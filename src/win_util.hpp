#pragma once

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <windows.h>
#include <wrl/client.h>

namespace snow
{
template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

inline void check(HRESULT result, const char *operation)
{
	if (FAILED(result)) {
		char code[32]{};
		std::snprintf(code, sizeof(code), " (HRESULT 0x%08lX)",
		              static_cast<unsigned long>(result));
		throw std::runtime_error(std::string(operation) + code);
	}
}

inline void check_win(BOOL success, const char *operation)
{
	if (!success) {
		const auto error = GetLastError();
		check(error ? HRESULT_FROM_WIN32(error) : E_FAIL, operation);
	}
}

inline std::wstring widen(const std::string &text)
{
	if (text.empty()) {
		return {};
	}
	const int n = MultiByteToWideChar(
		CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
	if (!n) {
		return L"Unknown error";
	}
	std::wstring result(static_cast<std::size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
	                    result.data(), n);
	return result;
}

class UniqueHandle {
  public:
	explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}

	~UniqueHandle()
	{
		reset();
	}

	UniqueHandle(const UniqueHandle &) = delete;
	UniqueHandle &operator=(const UniqueHandle &) = delete;

	HANDLE get() const noexcept
	{
		return value_;
	}

	void reset(HANDLE value = nullptr) noexcept
	{
		if (value_ && value_ != INVALID_HANDLE_VALUE) {
			CloseHandle(value_);
		}
		value_ = value;
	}

  private:
	HANDLE value_;
};

class ComApartment {
  public:
	ComApartment()
	{
		check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED),
		      "CoInitializeEx");
	}

	~ComApartment()
	{
		CoUninitialize();
	}

	ComApartment(const ComApartment &) = delete;
	ComApartment &operator=(const ComApartment &) = delete;
};
}
