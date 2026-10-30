// PlayShell: a small PNG reader for its own pictures (8-bit RGBA, not interlaced - tools/make_icons.py checks
// nothing else is embedded). Inflate: miniz (MIT). GPL-3.0.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "miniz.h"

namespace png {

inline uint32_t be32(const uint8_t* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

inline uint8_t paeth(int a, int b, int c) {
	const int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return static_cast<uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

// RGBA pixels (malloc'd, the caller frees), or nullptr.
inline uint8_t* decode(const uint8_t* data, size_t size, int* w_out, int* h_out) {
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	if (size < 33 || memcmp(data, sig, 8) != 0) {
		return nullptr;
	}
	uint32_t w = 0, h = 0;
	uint8_t* idat = nullptr;
	size_t   idat_len = 0;
	for (size_t at = 8; at + 12 <= size;) {
		const uint32_t len  = be32(data + at);
		const uint8_t* type = data + at + 4;
		const uint8_t* body = data + at + 8;
		if (at + 12 + len > size) {
			break;
		}
		if (memcmp(type, "IHDR", 4) == 0) {
			w = be32(body), h = be32(body + 4);
			if (body[8] != 8 || body[9] != 6 || body[12] != 0) { // 8-bit RGBA, not interlaced
				free(idat);
				return nullptr;
			}
		} else if (memcmp(type, "IDAT", 4) == 0) {
			uint8_t* more = static_cast<uint8_t*>(realloc(idat, idat_len + len));
			if (more == nullptr) {
				free(idat);
				return nullptr;
			}
			idat = more;
			memcpy(idat + idat_len, body, len);
			idat_len += len;
		} else if (memcmp(type, "IEND", 4) == 0) {
			break;
		}
		at += 12 + len;
	}
	if (w == 0 || h == 0 || w > 4096 || h > 4096 || idat == nullptr) {
		free(idat);
		return nullptr;
	}
	const size_t stride = size_t(w) * 4;
	mz_ulong     raw_len = mz_ulong((stride + 1) * h);
	uint8_t*     raw = static_cast<uint8_t*>(malloc(raw_len));
	uint8_t*     out = static_cast<uint8_t*>(malloc(stride * h));
	if (raw == nullptr || out == nullptr || mz_uncompress(raw, &raw_len, idat, mz_ulong(idat_len)) != MZ_OK ||
	    raw_len != (stride + 1) * h) {
		free(idat), free(raw), free(out);
		return nullptr;
	}
	free(idat);
	for (uint32_t y = 0; y < h; ++y) { // undo the row filters
		const uint8_t  f    = raw[y * (stride + 1)];
		const uint8_t* src  = raw + y * (stride + 1) + 1;
		uint8_t*       row  = out + y * stride;
		const uint8_t* prev = y > 0 ? out + (y - 1) * stride : nullptr;
		for (size_t x = 0; x < stride; ++x) {
			const int a = x >= 4 ? row[x - 4] : 0, b = prev != nullptr ? prev[x] : 0,
			          c = x >= 4 && prev != nullptr ? prev[x - 4] : 0;
			int v = src[x];
			switch (f) {
			case 1: v += a; break;
			case 2: v += b; break;
			case 3: v += (a + b) / 2; break;
			case 4: v += paeth(a, b, c); break;
			default: break;
			}
			row[x] = static_cast<uint8_t>(v);
		}
	}
	free(raw);
	*w_out = int(w), *h_out = int(h);
	return out;
}

} // namespace png
