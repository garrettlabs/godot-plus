/**************************************************************************/
/*  physx_blast_runtime.cpp                                               */
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

#include "physx_blast_runtime.h"

#include "core/error/error_macros.h"
#include "core/os/os.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

namespace PhysXBlastRuntime {

static bool available = true;

void load() {
#ifdef GODOT_PHYSX_BLAST_DELAYLOAD
	// Dependency order: NvBlastExtAuthoring and NvBlastExtShaders import NvBlast and NvBlastGlobals.
	static const char *libraries[] = { "NvBlastGlobals.dll", "NvBlast.dll", "NvBlastExtShaders.dll", "NvBlastExtAuthoring.dll" };
	String missing;
	for (const char *library : libraries) {
		// The reference is kept for the life of the process; the delay-load helper finds the module by name.
		if (OS::get_singleton()->load_runtime_library(library) == nullptr) {
			missing += missing.is_empty() ? String(library) : ", " + String(library);
		}
	}
	if (!missing.is_empty()) {
		available = false;
		const String library_dir = OS::get_singleton()->get_runtime_library_dir();
		const String where = library_dir.is_empty() ? String("next to the executable") : vformat("in %s or next to the executable", library_dir);
		ERR_PRINT(vformat("PhysX: Blast libraries not found %s: %s. Blast destruction (PhysXDestructible3D, PhysXBlastAuthoring) is unavailable.", where, missing));
	}
#endif
}

bool is_available() {
	return available;
}

} // namespace PhysXBlastRuntime
