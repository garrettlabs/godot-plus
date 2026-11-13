/**************************************************************************/
/*  material_shader_cache.h                                               */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/hash_map.h"
#include "core/templates/list.h"
#include "core/templates/rid.h"

// Frees a shader RID through the RenderingServer (defined in material.cpp).
void material_shader_cache_free_shader(RID p_shader);

// Generated material shaders are keyed by the material's settings and counted by user. When the last user goes
// away, the shader is kept (most recently unused first) so that the next material with the same settings does not
// generate and compile it again. Beyond MAX_UNUSED entries per class the least recently unused one is freed.
// All functions expect the caller to hold the map's lock, if the class has one.
// ShaderData needs `RID shader`, `int users` and `typename List<KeyT>::Element *unused_elem = nullptr`.
template <typename KeyT>
struct MaterialShaderLRU {
	static constexpr int MAX_UNUSED = 64;

	List<KeyT> unused;
	int max_unused = MAX_UNUSED;

	// Looks a key up and takes a user on it, taking it off the unused list if it was waiting there.
	template <typename MapT>
	auto *acquire(MapT &p_map, const KeyT &p_key) {
		auto *v = p_map.getptr(p_key);
		if (v) {
			v->users++;
			if (v->unused_elem) {
				unused.erase(v->unused_elem);
				v->unused_elem = nullptr;
			}
		}
		return v;
	}

	// Drops one user. At zero the shader moves to the unused list and the oldest ones beyond the cap are freed.
	template <typename MapT>
	void release(MapT &p_map, const KeyT &p_key) {
		auto *v = p_map.getptr(p_key);
		if (!v) {
			return;
		}
		v->users--;
		if (v->users == 0) {
			v->unused_elem = unused.push_front(p_key);
			trim(p_map, max_unused);
		}
	}

	template <typename MapT>
	void trim(MapT &p_map, int p_keep) {
		while (unused.size() > p_keep) {
			KeyT key = unused.back()->get();
			unused.pop_back();
			auto *v = p_map.getptr(key);
			if (v) {
				material_shader_cache_free_shader(v->shader);
				p_map.erase(key);
			}
		}
	}

	// At class shutdown: free every unused shader and stop caching, so materials freed later release theirs at once.
	template <typename MapT>
	void clear(MapT &p_map) {
		max_unused = 0;
		trim(p_map, 0);
	}
};
