/*
 * Copyright (c) 2007-2009 The Khronos Group Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and /or associated documentation files (the "Materials "), to
 * deal in the Materials without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Materials, and to permit persons to whom the Materials are
 * furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Materials.
 *
 * THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE MATERIALS OR THE USE OR OTHER DEALINGS IN THE
 * MATERIALS.
 *
 * OpenSLES_IID.c - OpenSL ES version 1.0
 *
 */


/* Exact Android 14 extension GUIDs. Declaring an ID does not implement its interface.
 * Source: https://android.googlesource.com/platform/frameworks/wilhelm/+/refs/tags/android-14.0.0_r1/src/OpenSLES_IID.cpp
 * Unknown/unsupported interfaces must still fail in the actual host backend. */
#include <SLES/OpenSLES.h>
static const struct SLInterfaceID_ android_guid_ANDROIDCONFIGURATION = { 0x89f6a7e0, 0xbeac, 0x11df, 0x8b5c, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDCONFIGURATION = &android_guid_ANDROIDCONFIGURATION;
static const struct SLInterfaceID_ android_guid_ANDROIDSIMPLEBUFFERQUEUE = { 0x198e4940, 0xc5d7, 0x11df, 0xa2a6, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = &android_guid_ANDROIDSIMPLEBUFFERQUEUE;
static const struct SLInterfaceID_ android_guid_ANDROIDACOUSTICECHOCANCELLATION = { 0x7b491460, 0x8d4d, 0x11e0, 0xbd61, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDACOUSTICECHOCANCELLATION = &android_guid_ANDROIDACOUSTICECHOCANCELLATION;
static const struct SLInterfaceID_ android_guid_ANDROIDAUTOMATICGAINCONTROL = { 0x0a8abfe0, 0x654c, 0x11e0, 0xba26, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDAUTOMATICGAINCONTROL = &android_guid_ANDROIDAUTOMATICGAINCONTROL;
static const struct SLInterfaceID_ android_guid_ANDROIDNOISESUPPRESSION = { 0x58b4b260, 0x8e06, 0x11e0, 0xaa8e, { 0x00, 0x02, 0xa5, 0xd5, 0xc5, 0x1b } };
static const SLInterfaceID shim_SL_IID_ANDROIDNOISESUPPRESSION = &android_guid_ANDROIDNOISESUPPRESSION;
