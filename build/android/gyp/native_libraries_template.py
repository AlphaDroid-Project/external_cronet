# Copyright 2019 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

NATIVE_LIBRARIES_TEMPLATE = """\
// This file is autogenerated by
//     build/android/gyp/write_native_libraries_java.py
// Please do not change its content.

package org.chromium.build;

public class NativeLibraries {{
    public static final int CPU_FAMILY_UNKNOWN = 0;
    public static final int CPU_FAMILY_ARM = 1;
    public static final int CPU_FAMILY_MIPS = 2;
    public static final int CPU_FAMILY_X86 = 3;

    // Set to true to enable the use of the Chromium Linker.
    public static {MAYBE_FINAL}boolean sUseLinker{USE_LINKER};
    public static {MAYBE_FINAL}boolean sUseLibraryInZipFile{USE_LIBRARY_IN_ZIP_FILE};
    public static {MAYBE_FINAL}boolean sUseModernLinker{USE_MODERN_LINKER};

    // This is the list of native libraries to be loaded (in the correct order)
    // by LibraryLoader.java.
    // TODO(cjhopman): This is public since it is referenced by NativeTestActivity.java
    // directly. The two ways of library loading should be refactored into one.
    public static {MAYBE_FINAL}String[] LIBRARIES = {{{LIBRARIES}}};

    // This is the expected version of the 'main' native library, which is the one that
    // implements the initial set of base JNI functions including
    // base::android::nativeGetVersionName()
    // TODO(torne): This is public to work around classloader issues in Trichrome
    // where NativeLibraries is not in the same dex as LibraryLoader.
    // We should instead split up Java code along package boundaries.
    public static {MAYBE_FINAL}String sVersionNumber = {VERSION_NUMBER};

    public static {MAYBE_FINAL}int sCpuFamily = {CPU_FAMILY};
}}
"""
