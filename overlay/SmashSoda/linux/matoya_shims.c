/* matoya.lib is built with MSVC and links against UCRT symbols that MinGW only
 * provides as inline functions. Export real definitions for them. */
#include <stdarg.h>
#include <stddef.h>

typedef struct _iobuf FILE;
extern int __mingw_vsnprintf(char *, size_t, const char *, va_list);
extern int __mingw_vfprintf(FILE *, const char *, va_list);
extern int _vsnwprintf(wchar_t *, size_t, const wchar_t *, va_list);

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int r = __mingw_vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
	return __mingw_vfprintf(f, fmt, ap);
}

int _snwprintf_s(wchar_t *buf, size_t size, size_t count, const wchar_t *fmt, ...)
{
	if (!buf || size == 0) return -1;
	size_t lim = count < size - 1 ? count : size - 1;
	va_list ap;
	va_start(ap, fmt);
	int r = _vsnwprintf(buf, lim, fmt, ap);
	va_end(ap);
	buf[(r < 0 || (size_t)r > lim) ? lim : (size_t)r] = L'\0';
	return r;
}
