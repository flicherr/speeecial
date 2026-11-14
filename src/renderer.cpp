#include "renderer.hpp"
#include "popup_order.hpp"
#include "win_util.hpp"
#include "window_policy.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_6.h>
#include <map>
#include <shobjidl.h>
#include <sstream>
#include <tuple>

namespace snow
{
namespace
{
constexpr auto overlay_class = L"SnowDesk.Overlay.1.0";
constexpr UINT sprite_size = 16;

LRESULT CALLBACK overlay_proc(HWND window, UINT message, WPARAM wparam,
                              LPARAM lparam)
{
	if (message == WM_NCCREATE) {
		const auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
		SetWindowLongPtrW(window, GWLP_USERDATA,
		                  reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}
	switch (message) {
	// WS_EX_LAYERED | WS_EX_TRANSPARENT provide cross-process input
	// transparency. HTTRANSPARENT alone does NOT provide that guarantee.
	case WM_NCHITTEST:
		return HTTRANSPARENT;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT:
		ValidateRect(window, nullptr);
		return 0;
	case WM_DPICHANGED:
		PostMessageW(
			reinterpret_cast<HWND>(GetWindowLongPtrW(window, GWLP_USERDATA)),
			message_rebuild, 0, 0);
		return 0;
	default:
		return DefWindowProcW(window, message, wparam, lparam);
	}
}

// A surface's BeginDraw returns a context already inside a draw transaction.
// Calling ID2D1DeviceContext::BeginDraw/EndDraw here would be incorrect.
class SurfaceDraw {
  public:
	SurfaceDraw(IDCompositionSurface *surface, UINT width, UINT height)
		: surface_(surface)
	{
		POINT offset{};
		check(surface_->BeginDraw(
				  nullptr, __uuidof(ID2D1DeviceContext),
				  reinterpret_cast<void **>(context_.GetAddressOf()), &offset),
		      "Surface BeginDraw");
		active_ = true;
		context_->SetDpi(96, 96);
		context_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
		context_->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
		context_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		context_->SetTransform(D2D1::Matrix3x2F::Translation(
			static_cast<float>(offset.x), static_cast<float>(offset.y)));
		context_->PushAxisAlignedClip(D2D1::RectF(0, 0,
		                                          static_cast<float>(width),
		                                          static_cast<float>(height)),
		                              D2D1_ANTIALIAS_MODE_ALIASED);
		context_->Clear(D2D1::ColorF(0, 0, 0, 0));
	}

	~SurfaceDraw()
	{
		if (active_) {
			context_->PopAxisAlignedClip();
			surface_->EndDraw();
		}
	}

	ID2D1DeviceContext *context() const noexcept
	{
		return context_.Get();
	}

	void finish()
	{
		context_->PopAxisAlignedClip();
		active_ = false;
		check(surface_->EndDraw(), "Surface EndDraw");
	}

  private:
	IDCompositionSurface *surface_;
	ComPtr<ID2D1DeviceContext> context_;
	bool active_ = false;
};

struct ParticleVisual {
	ComPtr<IDCompositionVisual> visual;
	std::uint32_t generation = 0;
	unsigned opacity = 8;
	bool shown = false;
};

struct MonitorScene {
	HWND window{};
	Display display;
	Simulation simulation;
	ComPtr<IDCompositionTarget> target;
	ComPtr<IDCompositionVisual> root, flakes, fragments, banks;
	std::vector<ParticleVisual> particles, debris;
	bool visible = false, active = false, yielded_z_order = false;
	float debris_idle = 0;
	ULONGLONG debris_used_at = 0;

	~MonitorScene()
	{
		if (window) {
			ShowWindow(window, SW_HIDE);
		}
		if (target) {
			target->SetRoot(nullptr);
		}
		target.Reset();
		root.Reset();
		flakes.Reset();
		fragments.Reset();
		banks.Reset();
		particles.clear();
		debris.clear();
		if (window) {
			RemovePropW(window, L"NonRudeHWND");
			DestroyWindow(window);
		}
	}
};

struct BankView {
	ComPtr<IDCompositionVisual> visual;
	Rect clip{};
	float x = 0, y = 0;
	bool positioned = false;
};

struct BankTile {
	float x = 0;
	UINT width = 0, height = 0;
	ComPtr<IDCompositionSurface> surface;
	std::vector<std::vector<BankView>> visuals;
};

struct BankGraphic {
	float width = 0;
	UINT height = 0;
	std::uint64_t profile_revision = ~std::uint64_t{0},
				  mask_signature = ~std::uint64_t{0};
	bool touched = false;
	std::vector<BankTile> tiles;
};

constexpr std::size_t bank_pixel_budget =
	4 * 1024 * 1024; // 16 MiB of uncompressed BGRA surfaces.
constexpr UINT bank_tile_width = 2048;
}

struct Renderer::Impl {
	HINSTANCE instance;
	HWND control;
	Settings settings;
	Landscape &landscape;
	ComPtr<ID3D11Device> d3d;
	ComPtr<ID2D1Factory1> d2d_factory;
	ComPtr<ID2D1Device> d2d;
	ComPtr<IDCompositionDevice> composition;
	ComPtr<IVirtualDesktopManager> desktops;
	std::array<ComPtr<IDCompositionSurface>, 3> sprites, chunk_sprites;
	std::array<ComPtr<IDCompositionEffectGroup>, 9> opacity_palette;
	std::vector<std::unique_ptr<MonitorScene>> monitors;
	std::vector<Simulation *> active_simulations;
	std::map<SurfaceId, BankGraphic> bank_graphics;
	std::vector<const SnowSurface *> candidates;
	std::wstring adapter_name;
	std::optional<GUID> desktop;
	std::optional<Rect> fullscreen_window;
	std::optional<Rect> foreground_cover;
	HWND foreground_window{};
	bool foreground_sampled = false;
	std::size_t bank_pixels = 0, skipped_banks = 0;
	float bank_elapsed = 1;
	bool geometry_dirty = true;
	std::uint64_t bank_draws = 0, commits = 0, z_order_changes = 0;
	std::size_t popup_count = 0;
	std::vector<HWND> popup_windows;
	std::vector<HWND> taskbar_windows;
	std::vector<PopupStackEntry> popup_stack;
	POINT last_cursor{};
	bool cursor_valid = false, capture_excluded = false,
		 desktop_follow_failed = false;
	ULONGLONG desktop_attempt = 0;

	Impl(HINSTANCE i, HWND c, Settings s, Landscape &world)
		: instance(i), control(c), settings(s), landscape(world)
	{
	}

	~Impl()
	{
		clear_scene();
		for (auto &sprite : sprites) {
			sprite.Reset();
		}
		for (auto &sprite : chunk_sprites) {
			sprite.Reset();
		}
		for (auto &effect : opacity_palette) {
			effect.Reset();
		}
		if (composition && SUCCEEDED(composition->Commit())) {
			composition->WaitForCommitCompletion();
		}
		composition.Reset();
		if (d2d) {
			d2d->ClearResources(0);
		}
		d2d.Reset();
		d2d_factory.Reset();
		flush_device();
		d3d.Reset();
	}

	void flush_device() noexcept
	{
		if (!d3d) {
			return;
		}
		ComPtr<ID3D11DeviceContext> context;
		d3d->GetImmediateContext(context.GetAddressOf());
		if (context) {
			context->ClearState();
			context->Flush();
		}
	}

	void clear_scene() noexcept
	{
		// Disconnect the retained tree while HWNDs and the DComp device still
		// exist. SetRoot/RemoveAllVisuals alone only enqueue a transaction.
		for (auto &s : monitors) {
			if (s->window) {
				ShowWindow(s->window, SW_HIDE);
			}
			if (s->target) {
				s->target->SetRoot(nullptr);
			}
			if (s->root) {
				s->root->RemoveAllVisuals();
			}
			if (s->flakes) {
				s->flakes->RemoveAllVisuals();
			}
			if (s->fragments) {
				s->fragments->RemoveAllVisuals();
			}
			if (s->banks) {
				s->banks->RemoveAllVisuals();
			}
		}
		if (composition && SUCCEEDED(composition->Commit())) {
			composition->WaitForCommitCompletion();
		}
		bank_graphics.clear();
		active_simulations.clear();
		candidates.clear();
		monitors.clear();
		if (composition && SUCCEEDED(composition->Commit())) {
			composition->WaitForCommitCompletion();
		}
		if (d2d) {
			d2d->ClearResources(0);
		}
		flush_device();
		bank_pixels = skipped_banks = 0;
		geometry_dirty = true;
		bank_elapsed = 1;
		popup_windows.clear();
		taskbar_windows.clear();
		popup_stack.clear();
		popup_count = 0;
		cursor_valid = false;
		desktop.reset();
	}

	void initialize_device()
	{
		ComPtr<IDXGIFactory1> factory;
		check(CreateDXGIFactory1(
				  __uuidof(IDXGIFactory1),
				  reinterpret_cast<void **>(factory.GetAddressOf())),
		      "CreateDXGIFactory1");
		ComPtr<IDXGIAdapter1> adapter;
		ComPtr<IDXGIFactory6> factory6;
		if (SUCCEEDED(factory.As(&factory6))) {
			factory6->EnumAdapterByGpuPreference(
				0, DXGI_GPU_PREFERENCE_MINIMUM_POWER, __uuidof(IDXGIAdapter1),
				reinterpret_cast<void **>(adapter.GetAddressOf()));
		}
		if (!adapter) {
			for (UINT index = 0;; ++index) {
				ComPtr<IDXGIAdapter1> candidate;
				if (factory->EnumAdapters1(index, candidate.GetAddressOf()) ==
				    DXGI_ERROR_NOT_FOUND) {
					break;
				}
				if (!candidate) {
					break;
				}
				DXGI_ADAPTER_DESC1 desc{};
				if (SUCCEEDED(candidate->GetDesc1(&desc)) &&
				    !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
					adapter = candidate;
					break;
				}
			}
		}
		if (!adapter) {
			throw std::runtime_error("No hardware DXGI adapter (software WARP "
			                         "fallback is intentionally disabled)");
		}
		DXGI_ADAPTER_DESC1 desc{};
		check(adapter->GetDesc1(&desc), "Get adapter description");
		adapter_name = desc.Description;
		constexpr D3D_FEATURE_LEVEL levels[] = {
			D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
			D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
		check(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		                        D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
		                        static_cast<UINT>(std::size(levels)),
		                        D3D11_SDK_VERSION, d3d.GetAddressOf(), nullptr,
		                        nullptr),
		      "D3D11CreateDevice");
		ComPtr<IDXGIDevice> dxgi;
		check(d3d.As(&dxgi), "Query IDXGIDevice");
		D2D1_FACTORY_OPTIONS options{};
		check(D2D1CreateFactory(
				  D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
				  &options,
				  reinterpret_cast<void **>(d2d_factory.GetAddressOf())),
		      "D2D1CreateFactory");
		check(d2d_factory->CreateDevice(dxgi.Get(), d2d.GetAddressOf()),
		      "Create D2D device");
		// Ask for the V1 interface to a V2 device; surfaces still support D2D
		// contexts.
		check(DCompositionCreateDevice2(
				  d2d.Get(), __uuidof(IDCompositionDevice),
				  reinterpret_cast<void **>(composition.GetAddressOf())),
		      "DCompositionCreateDevice2");
		// Share nine constant opacity effects (including zero) across all
		// particles; do not allocate a separate effect object for every
		// snowflake.
		for (std::size_t i = 0; i < opacity_palette.size(); ++i) {
			check(composition->CreateEffectGroup(
					  opacity_palette[i].GetAddressOf()),
			      "Create opacity effect");
			check(opacity_palette[i]->SetOpacity(static_cast<float>(i) / 8.0f),
			      "Set palette opacity");
		}
		for (UINT index = 0; index < sprites.size(); ++index) {
			check(composition->CreateSurface(sprite_size, sprite_size,
			                                 DXGI_FORMAT_B8G8R8A8_UNORM,
			                                 DXGI_ALPHA_MODE_PREMULTIPLIED,
			                                 sprites[index].GetAddressOf()),
			      "Create snowflake surface");
			SurfaceDraw draw(sprites[index].Get(), sprite_size, sprite_size);
			auto *dc = draw.context();
			ComPtr<ID2D1SolidColorBrush> brush;
			check(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1),
			                                brush.GetAddressOf()),
			      "Create sprite brush");
			const auto center = D2D1::Point2F(8, 8);
			if (index < 2) {
				for (int layer = 0; layer < 4; ++layer) {
					const float radius =
						6.5f - static_cast<float>(layer) * 1.2f;
					brush->SetColor(D2D1::ColorF(
						0.95f, 0.98f, 1,
						layer == 3 ? 0.85f : 0.08f + 0.08f * layer));
					dc->FillEllipse(
						D2D1::Ellipse(center, radius,
					                  radius * (index == 0 ? 1.0f : 0.8f)),
						brush.Get());
				}
			} else {
				brush->SetColor(D2D1::ColorF(0.97f, 0.99f, 1, 0.9f));
				for (int arm = 0; arm < 3; ++arm) {
					const float angle = static_cast<float>(arm) * 1.04719755f;
					const float x = std::cos(angle) * 5.5f,
								y = std::sin(angle) * 5.5f;
					dc->DrawLine(D2D1::Point2F(8 - x, 8 - y),
					             D2D1::Point2F(8 + x, 8 + y), brush.Get(),
					             1.25f);
				}
			}
			draw.finish();
		}
		for (unsigned index = 0; index < chunk_sprites.size(); ++index) {
			check(composition->CreateSurface(
					  sprite_size, sprite_size, DXGI_FORMAT_B8G8R8A8_UNORM,
					  DXGI_ALPHA_MODE_PREMULTIPLIED,
					  chunk_sprites[index].GetAddressOf()),
			      "Create chunk sprite");
			SurfaceDraw draw(chunk_sprites[index].Get(), sprite_size,
			                 sprite_size);
			auto *dc = draw.context();
			ComPtr<ID2D1PathGeometry> path;
			ComPtr<ID2D1GeometrySink> sink;
			check(d2d_factory->CreatePathGeometry(path.GetAddressOf()),
			      "Chunk path");
			check(path->Open(sink.GetAddressOf()), "Chunk outline");
			for (unsigned k = 0; k < 10; ++k) {
				const float angle = static_cast<float>(k) * 0.62831853f;
				const float radius =
					4.2f + static_cast<float>((k * 7 + index * 11) % 9) * 0.32f;
				const auto point = D2D1::Point2F(8 + std::cos(angle) * radius,
				                                 8 + std::sin(angle) * radius);
				if (k == 0) {
					sink->BeginFigure(point, D2D1_FIGURE_BEGIN_FILLED);
				} else {
					sink->AddLine(point);
				}
			}
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
			check(sink->Close(), "Close chunk outline");
			ComPtr<ID2D1SolidColorBrush> brush;
			check(dc->CreateSolidColorBrush(
					  D2D1::ColorF(0.83f, 0.90f, 0.97f, 0.97f),
					  brush.GetAddressOf()),
			      "Chunk brush");
			dc->FillGeometry(path.Get(), brush.Get());
			brush->SetColor(D2D1::ColorF(0.98f, 0.99f, 1.0f, 0.98f));
			dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(7, 6.8f), 3.5f, 2.5f),
			                brush.Get());
			draw.finish();
		}
	}

	void configure_particles(MonitorScene &scene)
	{
		scene.simulation.configure(
			screen_rect(scene.display.bounds), scene.display.scale,
			particles_per_monitor(settings.density, monitors.size()),
			max_debris / std::max(monitors.size(), std::size_t{1}));
		check(scene.flakes->RemoveAllVisuals(), "Remove snowflake visuals");
		scene.particles.clear();
		scene.particles.reserve(scene.simulation.particles().size());
		for (const auto &p : scene.simulation.particles()) {
			ParticleVisual v;
			check(composition->CreateVisual(v.visual.GetAddressOf()),
			      "Create snowflake visual");
			check(v.visual->SetContent(sprites[p.sprite].Get()),
			      "Set snowflake content");
			const float scale = p.size / static_cast<float>(sprite_size);
			check(v.visual->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale)),
			      "Scale snowflake");
			v.opacity = std::clamp(
				static_cast<unsigned>(std::lround(p.opacity * 8)), 1u, 8u);
			check(v.visual->SetEffect(opacity_palette[v.opacity].Get()),
			      "Snowflake opacity");
			v.shown = true;
			check(scene.flakes->AddVisual(v.visual.Get(), FALSE, nullptr),
			      "Attach snowflake");
			scene.particles.push_back(std::move(v));
		}
		scene.debris.resize(scene.simulation.debris().size());
		move_particles(scene);
	}

	bool particle_visible(float x, float y, float size) const noexcept
	{
		const float half = size * 0.5f;
		return settings.layer == Layer::front ||
		       !landscape.occluded({x - half, y - half, x + half, y + half});
	}

	void set_shown(ParticleVisual &v, bool show)
	{
		if (v.shown != show) {
			check(v.visual->SetEffect(
					  opacity_palette[show ? v.opacity : 0].Get()),
			      "Particle visibility");
			v.shown = show;
		}
	}

	void move_particles(MonitorScene &scene)
	{
		const auto particles = scene.simulation.particles();
		const auto &bounds = scene.display.bounds;
		for (std::size_t i = 0; i < particles.size(); ++i) {
			auto &v = scene.particles[i];
			const auto &p = particles[i];
			set_shown(v, particle_visible(p.x, p.y, p.size));
			if (!v.shown) {
				continue;
			}
			check(v.visual->SetOffsetX(p.x - p.size * 0.5f -
			                           static_cast<float>(bounds.left)),
			      "Move snowflake X");
			check(v.visual->SetOffsetY(p.y - p.size * 0.5f -
			                           static_cast<float>(bounds.top)),
			      "Move snowflake Y");
		}
		const auto debris = scene.simulation.debris();
		for (std::size_t i = 0; i < debris.size(); ++i) {
			auto &v = scene.debris[i];
			const auto &p = debris[i];
			if (!p.active || p.delay > 0) {
				if (v.visual) {
					set_shown(v, false);
				}
				continue;
			}
			if (!v.visual) {
				check(composition->CreateVisual(v.visual.GetAddressOf()),
				      "Create fragment visual");
				check(v.visual->SetContent(chunk_sprites[p.sprite].Get()),
				      "Fragment content");
				check(v.visual->SetEffect(opacity_palette[0].Get()),
				      "Initialize fragment opacity");
				check(scene.fragments->AddVisual(v.visual.Get(), TRUE, nullptr),
				      "Attach fragment");
				v.shown = false;
			}
			if (v.generation != p.generation) {
				v.generation = p.generation;
				check(v.visual->SetContent(chunk_sprites[p.sprite].Get()),
				      "Fragment shape");
			}
			set_shown(v, particle_visible(p.x, p.y, p.size));
			if (!v.shown) {
				continue;
			}
			const float scale = p.size / static_cast<float>(sprite_size);
			const auto transform =
				D2D1::Matrix3x2F::Translation(-8, -8) *
				D2D1::Matrix3x2F::Scale(scale * p.aspect, scale) *
				D2D1::Matrix3x2F::Rotation(p.angle);
			check(v.visual->SetTransform(transform), "Rotate falling chunk");
			check(v.visual->SetOffsetX(p.x - static_cast<float>(bounds.left)),
			      "Move fragment X");
			check(v.visual->SetOffsetY(p.y - static_cast<float>(bounds.top)),
			      "Move fragment Y");
		}
	}

	void read_popup_stack(std::span<const HWND> popups,
	                      std::span<const HWND> taskbars)
	{
		popup_stack.clear();
		if (popups.empty() && taskbars.empty()) {
			return;
		}
		std::array<HWND, 160> pending{};
		std::size_t remaining = 0;
		const auto add_candidate = [&](HWND popup) {
			RECT bounds{};
			if (remaining < pending.size() && IsWindowVisible(popup) &&
			    !IsIconic(popup) && GetWindowRect(popup, &bounds) &&
			    screen_rect(bounds).valid() &&
			    std::any_of(
					monitors.begin(), monitors.end(),
					[&](const auto &s) {
						return screen_rect(bounds).intersects(
							screen_rect(s->display.bounds));
					})) {
				pending[remaining++] = popup;
			}
		};
		for (HWND popup : popups) {
			add_candidate(popup);
		}
		for (HWND bar : taskbars) {
			// The shell, not us, controls when the taskbar is above fullscreen
			// UI. Never follow a demoted taskbar beneath a game with the whole
			// scene.
			if (GetWindowLongPtrW(bar, GWL_EXSTYLE) & WS_EX_TOPMOST) {
				add_candidate(bar);
			}
		}
		// The worker's list identifies eligible popups, not their CURRENT
		// order. Tooltip HWNDs are reused/reordered between icons. Only cheap
		// User32 queries run here; DWM/COM and the full desktop scan stay on
		// the worker.
		unsigned visited = 0;
		for (HWND w = GetTopWindow(nullptr); w && remaining && visited < 512;
		     w = GetWindow(w, GW_HWNDNEXT), ++visited) {
			if (auto found = std::find(pending.begin(), pending.end(), w);
			    found != pending.end()) {
				*found = nullptr;
				--remaining;
			}
			if (!IsWindowVisible(w) || IsIconic(w)) {
				continue;
			}
			RECT bounds{};
			if (!GetWindowRect(w, &bounds) || !screen_rect(bounds).valid() ||
			    std::none_of(monitors.begin(), monitors.end(),
			                 [&](const auto &s) {
								 return screen_rect(bounds).intersects(
									 screen_rect(s->display.bounds));
							 })) {
				continue;
			}
			wchar_t name[128]{};
			GetClassNameW(w, name, 128);
			const std::wstring_view class_name{name};
			DWORD pid = 0;
			GetWindowThreadProcessId(w, &pid);
			if ((pid == GetCurrentProcessId() && class_name == overlay_class) ||
			    class_name == L"Progman" || class_name == L"WorkerW") {
				continue;
			}
			const auto style = GetWindowLongPtrW(w, GWL_STYLE),
					   ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
			if (style & WS_CHILD) {
				continue;
			}
			const bool taskbar = class_name == L"Shell_TrayWnd" ||
			                     class_name == L"Shell_SecondaryTrayWnd";
			const bool taskbar_anchor =
				taskbar && (ex & WS_EX_TOPMOST) &&
				std::find(taskbars.begin(), taskbars.end(), w) !=
					taskbars.end();
			if (taskbar && !taskbar_anchor) {
				continue;
			}
			// Validate the role again: a stale HWND may have been reused by an
			// app.
			const WindowTraits traits{class_name,
			                          (style & WS_POPUP) != 0,
			                          (style & WS_CAPTION) == WS_CAPTION,
			                          (style & WS_THICKFRAME) != 0,
			                          (ex & WS_EX_TOOLWINDOW) != 0,
			                          (ex & WS_EX_NOACTIVATE) != 0,
			                          (ex & WS_EX_APPWINDOW) != 0,
			                          GetWindow(w, GW_OWNER) != nullptr};
			const UINT dpi = GetDpiForWindow(w);
			const bool status_bubble =
				browser_status_bubble(w, traits, screen_rect(bounds),
			                          dpi ? static_cast<float>(dpi) / 96 : 1);
			const bool popup =
				taskbar_anchor ||
				(!status_bubble &&
			     std::find(popups.begin(), popups.end(), w) != popups.end() &&
			     is_native_popup(traits));
			if (!popup && (ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT)) {
				continue;
			}
			popup_stack.push_back({reinterpret_cast<std::uintptr_t>(w),
			                       screen_rect(bounds), popup});
			// Nothing below the first application on each monitor can be an
			// anchor.
			if (!popup &&
			    std::all_of(
					monitors.begin(), monitors.end(), [&](const auto &s) {
						return std::any_of(
							popup_stack.begin(), popup_stack.end(),
							[&](const auto &entry) {
								return !entry.popup &&
					                   entry.bounds.intersects(
										   screen_rect(s->display.bounds));
							});
					})) {
				break;
			}
		}
		// A changing/malformed Z chain must never lead to a speculative
		// demotion.
		if (visited == 512) {
			popup_stack.clear();
		}
	}

	void order_below_popups(std::span<const HWND> popups)
	{
		popup_count = popups.size();
		read_popup_stack(popups, taskbar_windows);
		for (auto &s : monitors) {
			HWND anchor = reinterpret_cast<HWND>(
				popup_anchor(popup_stack, screen_rect(s->display.bounds)));
			if (anchor && (!IsWindowVisible(anchor) || IsIconic(anchor))) {
				anchor = nullptr;
			}
			const bool topmost = (GetWindowLongPtrW(s->window, GWL_EXSTYLE) &
			                      WS_EX_TOPMOST) != 0;
			if (!anchor && !s->yielded_z_order && topmost) {
				continue;
			}
			HWND previous = GetWindow(s->window, GW_HWNDPREV);
			for (unsigned n = 0; previous && n < 128; ++n) {
				if (std::none_of(monitors.begin(), monitors.end(),
				                 [&](const auto &other) {
									 return other->window == previous;
								 })) {
					break;
				}
				previous = GetWindow(previous, GW_HWNDPREV);
			}
			const bool anchor_topmost =
				anchor &&
				(GetWindowLongPtrW(anchor, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
			if (anchor && previous == anchor && topmost == anchor_topmost) {
				s->yielded_z_order = true;
				continue;
			}
			// A non-topmost anchor is allowed only ABOVE the first application.
			// Stay below shell taskbars and native menus/tooltips, but never
			// follow a popup underneath an application with the entire
			// monitor-sized snow layer.
			if (SetWindowPos(s->window, anchor ? anchor : HWND_TOPMOST, 0, 0, 0,
			                 0,
			                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
			                     SWP_NOOWNERZORDER)) {
				++z_order_changes;
				s->yielded_z_order = anchor != nullptr;
			}
		}
	}

	void create_monitors(const std::vector<Display> &displays)
	{
		WNDCLASSEXW wc{sizeof(wc)};
		wc.lpfnWndProc = overlay_proc;
		wc.hInstance = instance;
		wc.lpszClassName = overlay_class;
		if (!RegisterClassExW(&wc) &&
		    GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			check_win(FALSE, "Register overlay class");
		}
		for (const auto &display : displays) {
			auto s = std::make_unique<MonitorScene>();
			s->display = display;
			s->simulation =
				Simulation(0x534e4f57u +
			               static_cast<std::uint32_t>(monitors.size()) * 7919u);
			const auto &b = display.bounds;
			const DWORD style = WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
			                    WS_EX_NOACTIVATE | WS_EX_LAYERED |
			                    WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP;
			s->window = CreateWindowExW(
				style, overlay_class, L"speeecial (snow desktop)",
				WS_POPUP | WS_DISABLED, b.left, b.top, b.right - b.left,
				b.bottom - b.top, nullptr, nullptr, instance, control);
			if (!s->window) {
				check_win(FALSE, "Create overlay");
			}
			// A screen-sized decoration must not trigger Explorer's fullscreen
			// heuristics. FALSE in MarkFullscreenWindow does NOT opt out of
			// them. Set this before the first ShowWindow; it survives hide/show
			// cycles.
			check_win(
				SetPropW(s->window, L"NonRudeHWND",
			             reinterpret_cast<HANDLE>(static_cast<INT_PTR>(1))),
				"Exclude decorative overlay from shell fullscreen detection");
			check_win(SetLayeredWindowAttributes(s->window, 0, 255, LWA_ALPHA),
			          "Initialize layered overlay");
			check(composition->CreateTargetForHwnd(s->window, TRUE,
			                                       s->target.GetAddressOf()),
			      "Create composition target");
			check(composition->CreateVisual(s->root.GetAddressOf()),
			      "Create root");
			check(composition->CreateVisual(s->flakes.GetAddressOf()),
			      "Create flakes group");
			check(composition->CreateVisual(s->banks.GetAddressOf()),
			      "Create banks group");
			check(composition->CreateVisual(s->fragments.GetAddressOf()),
			      "Create fragments group");
			check(s->root->AddVisual(s->flakes.Get(), TRUE, nullptr),
			      "Attach flakes group");
			check(s->root->AddVisual(s->banks.Get(), TRUE, nullptr),
			      "Attach banks group");
			check(s->root->AddVisual(s->fragments.Get(), TRUE, nullptr),
			      "Attach fragments group");
			check(s->target->SetRoot(s->root.Get()), "Set root");
			monitors.push_back(std::move(s));
		}
		candidates.reserve(max_surfaces);
		active_simulations.reserve(monitors.size());
		if (!desktops) {
			CoCreateInstance(
				CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
				__uuidof(IVirtualDesktopManager),
				reinterpret_cast<void **>(desktops.GetAddressOf()));
		}
		for (auto &s : monitors) {
			configure_particles(*s);
		}
		refresh_banks(true);
		check(composition->Commit(), "Initial commit");
	}

	void remove_bank(BankGraphic &graphic)
	{
		for (auto &tile : graphic.tiles) {
			for (std::size_t i = 0; i < tile.visuals.size(); ++i) {
				for (auto &view : tile.visuals[i]) {
					check(monitors[i]->banks->RemoveVisual(view.visual.Get()),
					      "Remove bank tile");
				}
			}
		}
		graphic.tiles.clear();
	}

	bool onscreen(Rect r) const noexcept
	{
		for (const auto &s : monitors) {
			if (r.intersects(screen_rect(s->display.bounds))) {
				return true;
			}
		}
		return false;
	}

	void position_bank(const SnowSurface &bank, BankGraphic &graphic)
	{
		for (auto &tile : graphic.tiles) {
			const float gx = bank.snapshot.rect.left + tile.x,
						gy = bank.snapshot.rect.top -
			                 static_cast<float>(tile.height);
			for (std::size_t i = 0; i < monitors.size(); ++i) {
				auto &scene = *monitors[i];
				auto &views = tile.visuals[i];
				const auto bounds = screen_rect(scene.display.bounds);
				const Rect monitor{bounds.left - gx, bounds.top - gy,
				                   bounds.right - gx, bounds.bottom - gy};
				std::size_t used = 0;
				for (auto region : bank.visible_regions) {
					region.left -= tile.x;
					region.right -= tile.x;
					const auto clip = intersect(
						intersect(region, {0, 0, static_cast<float>(tile.width),
					                       static_cast<float>(tile.height)}),
						monitor);
					if (!clip.valid()) {
						continue;
					}
					if (used == views.size()) {
						BankView view;
						check(composition->CreateVisual(
								  view.visual.GetAddressOf()),
						      "Create clipped bank visual");
						check(view.visual->SetContent(tile.surface.Get()),
						      "Set bank content");
						check(scene.banks->AddVisual(view.visual.Get(), TRUE,
						                             nullptr),
						      "Attach clipped bank");
						views.push_back(std::move(view));
					}
					auto &view = views[used++];
					if (view.clip != clip) {
						check(
							view.visual->SetClip(D2D1::RectF(
								clip.left, clip.top, clip.right, clip.bottom)),
							"Clip bank");
						view.clip = clip;
					}
					const float x = gx - bounds.left, y = gy - bounds.top;
					if (!view.positioned || view.x != x || view.y != y) {
						check(view.visual->SetOffsetX(x), "Position bank X");
						check(view.visual->SetOffsetY(y), "Position bank Y");
						view.x = x;
						view.y = y;
						view.positioned = true;
					}
				}
				while (views.size() > used) {
					check(scene.banks->RemoveVisual(views.back().visual.Get()),
					      "Remove hidden bank region");
					views.pop_back();
				}
			}
		}
		graphic.mask_signature = bank.mask_signature;
	}

	void draw_bank(const SnowSurface &bank, BankGraphic &graphic)
	{
		++bank_draws;
		for (auto &tile : graphic.tiles) {
			SurfaceDraw draw(tile.surface.Get(), tile.width, tile.height);
			auto *dc = draw.context();
			ComPtr<ID2D1PathGeometry> path;
			ComPtr<ID2D1GeometrySink> sink;
			check(d2d_factory->CreatePathGeometry(path.GetAddressOf()),
			      "Create bank path");
			check(path->Open(sink.GetAddressOf()), "Open bank path");
			const float base = static_cast<float>(tile.height),
						right = tile.x + static_cast<float>(tile.width);
			sink->BeginFigure(D2D1::Point2F(0, base), D2D1_FIGURE_BEGIN_FILLED);
			sink->AddLine(D2D1::Point2F(0, base - bank.snow.height_at(tile.x)));
			// Global sampling avoids seams at texture tile boundaries. Two-DIP
			// intervals also resolve the curved shoulders at small bank sizes.
			const float step = std::max(1.0f, 2 * bank.snapshot.scale);
			for (float x = (std::floor(tile.x / step) + 1) * step; x < right;
			     x += step) {
				sink->AddLine(
					D2D1::Point2F(x - tile.x, base - bank.snow.height_at(x)));
			}
			sink->AddLine(D2D1::Point2F(static_cast<float>(tile.width),
			                            base - bank.snow.height_at(std::min(
												   right, bank.snow.width()))));
			sink->AddLine(D2D1::Point2F(static_cast<float>(tile.width), base));
			sink->EndFigure(D2D1_FIGURE_END_CLOSED);
			check(sink->Close(), "Close bank path");
			ComPtr<ID2D1SolidColorBrush> brush;
			check(
				dc->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.97f, 1, 0.98f),
			                              brush.GetAddressOf()),
				"Create bank brush");
			dc->FillGeometry(path.Get(), brush.Get());

			draw.finish();
		}
		graphic.profile_revision = bank.snow.revision();
		graphic.mask_signature = bank.mask_signature;
	}

	void refresh_banks(bool force)
	{
		candidates.clear();
		for (const auto &[id, bank] : landscape.surfaces()) {
			(void)id;
			if (bank.enabled(settings) && !bank.snow.empty() &&
			    onscreen(bank.snow_bounds()) && !bank.visible_regions.empty()) {
				candidates.push_back(&bank);
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](auto a, auto b) {
			return a->snapshot.z_order < b->snapshot.z_order;
		});
		for (auto &[id, g] : bank_graphics) {
			(void)id;
			g.touched = false;
		}
		bank_pixels = 0;
		skipped_banks = 0;
		for (const auto *bank : candidates) {
			const UINT height =
				static_cast<UINT>(std::ceil(bank->snow.max_height())) + 2;
			const UINT width = static_cast<UINT>(std::ceil(bank->snow.width()));
			const auto pixels = static_cast<std::size_t>(width) * height;
			if (pixels > bank_pixel_budget - bank_pixels) {
				++skipped_banks;
				continue;
			}
			bank_pixels += pixels;
			auto &graphic = bank_graphics[bank->snapshot.id];
			graphic.touched = true;
			if (graphic.width != bank->snow.width() ||
			    graphic.height != height || graphic.tiles.empty()) {
				remove_bank(graphic);
				graphic.width = bank->snow.width();
				graphic.height = height;
				// Tiles stay shared across monitors, keeping seams continuous.
				for (UINT x = 0; x < width; x += bank_tile_width) {
					BankTile tile;
					tile.x = static_cast<float>(x);
					tile.width = std::min(bank_tile_width, width - x);
					tile.height = height;
					check(composition->CreateSurface(
							  tile.width, tile.height,
							  DXGI_FORMAT_B8G8R8A8_UNORM,
							  DXGI_ALPHA_MODE_PREMULTIPLIED,
							  tile.surface.GetAddressOf()),
					      "Create bank tile surface");
					tile.visuals.resize(monitors.size());
					graphic.tiles.push_back(std::move(tile));
				}
				graphic.profile_revision = ~std::uint64_t{0};
			}
			position_bank(*bank, graphic);
			if (graphic.profile_revision == ~std::uint64_t{0} ||
			    ((force || bank_elapsed >= 0.125f) &&
			     graphic.profile_revision != bank->snow.revision())) {
				draw_bank(*bank, graphic);
			}
		}
		for (auto i = bank_graphics.begin(); i != bank_graphics.end();) {
			if (!i->second.touched) {
				remove_bank(i->second);
				i = bank_graphics.erase(i);
			} else {
				++i;
			}
		}
		geometry_dirty = false;
		if (bank_elapsed >= 0.125f) {
			bank_elapsed = 0;
		}
	}

	void emit_pending()
	{
		for (const auto &e : landscape.emissions()) {
			for (auto &scene : monitors) {
				// The bank can start just above the physical screen. Only emit
				// on a running monitor, including hide-only pause; never queue
				// stopped bursts.
				auto b = screen_rect(scene->display.bounds);
				b.top -= 64 * scene->display.scale;
				if (scene->active && b.contains(e.x, e.y)) {
					scene->simulation.emit(e);
					scene->debris_idle = 0;
					scene->debris_used_at = GetTickCount64();
					break;
				}
			}
		}
		landscape.consume_emissions();
	}

	void follow_desktop(const std::optional<GUID> &next)
	{
		if (!desktops || !next) {
			return;
		}
		if (desktop && IsEqualGUID(*desktop, *next) && !desktop_follow_failed) {
			return;
		}
		const auto now = GetTickCount64();
		if (desktop_follow_failed && now - desktop_attempt < 1000) {
			return;
		}
		desktop_attempt = now;
		desktop_follow_failed = false;
		for (auto &s : monitors) {
			GUID current{};
			if (SUCCEEDED(desktops->GetWindowDesktopId(s->window, &current)) &&
			    !IsEqualGUID(current, *next)) {
				if (FAILED(desktops->MoveWindowToDesktop(s->window, *next))) {
					desktop_follow_failed = true;
				}
			}
		}
		desktop = next;
	}
};

Renderer::Renderer(HINSTANCE instance, HWND control,
                   const std::vector<Display> &displays, Settings settings,
                   Landscape &world)
	: impl_(std::make_unique<Impl>(instance, control, settings, world))
{
	impl_->initialize_device();
	impl_->create_monitors(displays);
}

Renderer::~Renderer() = default;

void Renderer::rebuild_scene(const std::vector<Display> &displays,
                             Settings settings)
{
	check_device();
	impl_->clear_scene();
	impl_->settings = settings;
	impl_->create_monitors(displays);
}

void Renderer::set_settings(Settings settings)
{
	const bool density_changed = impl_->settings.density != settings.density;
	impl_->settings = settings;
	for (auto &s : impl_->monitors) {
		if (density_changed) {
			impl_->configure_particles(*s);
		} else {
			impl_->move_particles(*s);
		}
	}
	impl_->refresh_banks(true);
	check(impl_->composition->Commit(), "Apply settings");
}

bool Renderer::set_capture(bool show) noexcept
{
	bool ok = true;
	for (auto &s : impl_->monitors) {
		if (!SetWindowDisplayAffinity(s->window,
		                              show ? WDA_NONE : 0x00000011)) {
			ok = false;
		}
	}
	if (!ok) {
		for (auto &s : impl_->monitors) {
			SetWindowDisplayAffinity(s->window, WDA_NONE);
		}
	}
	impl_->capture_excluded = ok && !show;
	return ok;
}

void Renderer::synchronize_desktop(const DesktopSnapshot &snapshot,
                                   bool defer_to_frame)
{
	refresh_foreground(true);
	impl_->fullscreen_window = snapshot.fullscreen_window;
	impl_->follow_desktop(snapshot.desktop);
	impl_->popup_windows = snapshot.popups;
	impl_->taskbar_windows = snapshot.taskbars;
	impl_->order_below_popups(impl_->popup_windows);
	impl_->emit_pending();
	impl_->geometry_dirty = true;
	// Active frames submit particles and geometry together exactly once.
	// A static scene still needs to react when its supporting window changes.
	if (!defer_to_frame && !needs_animation() &&
	    std::any_of(impl_->monitors.begin(), impl_->monitors.end(),
	                [](const auto &s) { return s->visible; })) {
		impl_->refresh_banks(false);
		check(impl_->composition->Commit(), "Synchronize static desktop");
		++impl_->commits;
	}
}

void Renderer::refresh_foreground(bool force)
{
	const HWND window = GetForegroundWindow();
	// A temporary null foreground during Alt+Tab is not a taskbar reveal.
	if (!window || (!force && impl_->foreground_sampled &&
	                window == impl_->foreground_window)) {
		return;
	}
	const auto cover = foreground_fullscreen_fast(window);
	if (GetForegroundWindow() != window) {
		return; // Focus changed while sampling; the next event/frame retries.
	}
	const bool changed = !impl_->foreground_sampled ||
	                     window != impl_->foreground_window ||
	                     cover != impl_->foreground_cover;
	impl_->foreground_sampled = true;
	impl_->foreground_window = window;
	impl_->foreground_cover = cover;
	// Do not re-arm on every desktop snapshot: after the shell acknowledges
	// hiding, hovering/revealing the taskbar must work with the same
	// foreground.
	if (!changed || !impl_->landscape.begin_foreground_transition(cover)) {
		return;
	}
	impl_->geometry_dirty = true;
	if (std::any_of(impl_->monitors.begin(), impl_->monitors.end(),
	                [](const auto &s) { return s->visible; })) {
		// Submit visibility immediately, including static scenes. Neither the
		// worker mailbox nor the 8 Hz profile repaint interval gates this path.
		impl_->refresh_banks(false);
		check(impl_->composition->Commit(), "Apply foreground bank visibility");
		++impl_->commits;
	}
}

void Renderer::update_policy(bool suspended, bool hide_effects)
{
	bool reveal = false;
	for (auto &s : impl_->monitors) {
		const auto monitor = screen_rect(s->display.bounds);
		const auto fullscreen = impl_->fullscreen_window;
		const bool fullscreen_here = fullscreen &&
		                             fullscreen->left <= monitor.left + 2 &&
		                             fullscreen->top <= monitor.top + 2 &&
		                             fullscreen->right >= monitor.right - 2 &&
		                             fullscreen->bottom >= monitor.bottom - 2;
		s->active = !suspended &&
		            !(impl_->settings.pause_fullscreen && fullscreen_here);
		const bool show = s->active && !hide_effects;
		if (show && !s->visible) {
			impl_->move_particles(*s);
			reveal = true;
		}
		if (!show && s->visible) {
			ShowWindow(s->window, SW_HIDE);
		}
		s->visible = show;
		// Stopping freezes debris too. Idle GPU visuals can still be released.
		if (s->debris_used_at && GetTickCount64() - s->debris_used_at > 10000 &&
		    s->simulation.active_debris() == 0) {
			check(s->fragments->RemoveAllVisuals(),
			      "Release idle fragment visuals");
			for (auto &v : s->debris) {
				v = ParticleVisual{};
			}
			s->debris_used_at = 0;
		}
	}
	if (suspended || hide_effects) {
		impl_->cursor_valid = false;
	}
	if (reveal) {
		impl_->refresh_banks(true);
		check(impl_->composition->Commit(), "Reveal current scene");
		++impl_->commits;
		for (auto &s : impl_->monitors) {
			if (s->visible) {
				ShowWindow(s->window, SW_SHOWNOACTIVATE);
			}
		}
		impl_->order_below_popups(impl_->popup_windows);
	}
}

void Renderer::tick(float dt)
{
	Cursor cursor;
	POINT point{};
	if (impl_->settings.cursor && GetCursorPos(&point)) {
		cursor.x = static_cast<float>(point.x);
		cursor.y = static_cast<float>(point.y);
		if (impl_->cursor_valid && dt > 0.001f && dt < 0.1f) {
			cursor.vx = static_cast<float>(point.x - impl_->last_cursor.x) / dt;
			cursor.vy = static_cast<float>(point.y - impl_->last_cursor.y) / dt;
			cursor.active =
				std::hypot(static_cast<float>(point.x - impl_->last_cursor.x),
			               static_cast<float>(point.y - impl_->last_cursor.y)) <
				700;
		}
		impl_->last_cursor = point;
		impl_->cursor_valid = true;
	}
	impl_->active_simulations.clear();
	for (auto &s : impl_->monitors) {
		if (s->active) {
			s->simulation.update(dt, impl_->settings, impl_->landscape, cursor);
			impl_->active_simulations.push_back(&s->simulation);
		}
	}
	if (impl_->active_simulations.empty()) {
		return;
	}
	for (auto *simulation : impl_->active_simulations) {
		simulation->transfer_outside(impl_->active_simulations);
	}
	for (auto &s : impl_->monitors) {
		if (!s->visible) {
			continue;
		}
		impl_->move_particles(*s);
		if (s->simulation.active_debris() == 0) {
			s->debris_idle += dt;
		} else {
			s->debris_idle = 0;
		}
		if (s->debris_idle > 10) {
			check(s->fragments->RemoveAllVisuals(),
			      "Release idle fragment pool");
			for (auto &v : s->debris) {
				v = ParticleVisual{};
			}
			s->debris_idle = 0;
		}
	}
	impl_->landscape.update(dt, impl_->settings);
	impl_->bank_elapsed += dt;
	// Hide-only pause continues physics without submitting invisible GPU
	// frames.
	if (std::none_of(impl_->monitors.begin(), impl_->monitors.end(),
	                 [](const auto &s) { return s->visible; })) {
		return;
	}
	// The final fragment may deposit snow just before the frame timer stops.
	// Flush that last profile even when the normal repaint interval has not
	// elapsed.
	const bool stopping = !needs_animation();
	if (impl_->bank_elapsed >= 0.125f || impl_->geometry_dirty || stopping) {
		impl_->refresh_banks(stopping);
	}
	check(impl_->composition->Commit(), "Frame commit");
	++impl_->commits;
}

void Renderer::clear_snow()
{
	impl_->landscape.clear_snow();
	for (auto &s : impl_->monitors) {
		s->simulation.clear_debris();
		impl_->move_particles(*s);
	}
	impl_->refresh_banks(true);
	check(impl_->composition->Commit(), "Clear snow");
}

void Renderer::hide() noexcept
{
	for (auto &s : impl_->monitors) {
		ShowWindow(s->window, SW_HIDE);
		s->visible = false;
		s->active = false;
	}
}

void Renderer::check_device() const
{
	check(impl_->d3d->GetDeviceRemovedReason(), "Graphics device removed");
	BOOL valid = FALSE;
	check(impl_->composition->CheckDeviceState(&valid),
	      "Check composition device");
	if (!valid) {
		throw std::runtime_error("DirectComposition device needs recreation");
	}
}

bool Renderer::needs_animation() const noexcept
{
	for (const auto &s : impl_->monitors) {
		if (s->active && (!s->simulation.particles().empty() ||
		                  s->simulation.active_debris() > 0 ||
		                  impl_->landscape.needs_melting(impl_->settings))) {
			return true;
		}
	}
	return false;
}

std::size_t Renderer::particle_count() const noexcept
{
	std::size_t count = 0;
	for (const auto &s : impl_->monitors) {
		count +=
			s->simulation.particles().size() + s->simulation.active_debris();
	}
	return count;
}

std::string Renderer::description() const
{
	std::ostringstream out;
	const int size =
		WideCharToMultiByte(CP_UTF8, 0, impl_->adapter_name.c_str(), -1,
	                        nullptr, 0, nullptr, nullptr);
	std::string adapter(static_cast<std::size_t>(std::max(size, 1)), '\0');
	WideCharToMultiByte(CP_UTF8, 0, impl_->adapter_name.c_str(), -1,
	                    adapter.data(), size, nullptr, nullptr);
	out << "Renderer: Direct2D + DirectComposition shared surfaces\nGPU: "
		<< adapter.c_str() << "\nMonitors: " << impl_->monitors.size()
		<< "\nActive particles + fragments: " << particle_count()
		<< "\nRetained profiles: " << impl_->landscape.surfaces().size()
		<< "\nBank texture redraws: " << impl_->bank_draws
		<< "\nFrame/static commits: " << impl_->commits
		<< "\nNative popups: " << impl_->popup_count
		<< "\nOverlay Z-order changes: " << impl_->z_order_changes
		<< "\nShell taskbars tracked: " << impl_->taskbar_windows.size()
		<< "\nRendered banks: " << impl_->bank_graphics.size()
		<< "\nBank BGRA pixel bytes: " << impl_->bank_pixels * 4
		<< "\nBanks omitted by texture budget: " << impl_->skipped_banks
		<< "\nCapture exclusion active: " << impl_->capture_excluded
		<< "\nDesktop follow failed: " << impl_->desktop_follow_failed << '\n';
	for (const auto &s : impl_->monitors) {
		const auto &b = s->display.bounds;
		out << "  " << b.left << ',' << b.top << " / " << b.right - b.left
			<< 'x' << b.bottom - b.top << " / DPI " << s->display.scale
			<< " / visible " << s->visible << " / simulation " << s->active
			<< " / NonRude " << (GetPropW(s->window, L"NonRudeHWND") != nullptr)
			<< " / disabled " << !IsWindowEnabled(s->window) << " / topmost "
			<< ((GetWindowLongPtrW(s->window, GWL_EXSTYLE) & WS_EX_TOPMOST) !=
		        0)
			<< '\n';
	}
	return out.str();
}
}
