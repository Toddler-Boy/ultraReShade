/*
 * Copyright (C) 2022 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hook_manager.hpp"

#define ID3D10Resource_GetDevice reshade::hooks::call_vtable<3, HRESULT, ID3D10Resource, ID3D10Device **>

#define ID3D10Buffer_Map reshade::hooks::call_vtable<10, HRESULT, ID3D10Buffer, D3D10_MAP, UINT, void **>
#define ID3D10Buffer_Unmap reshade::hooks::call_vtable<11, HRESULT, ID3D10Buffer>

#define ID3D10Texture1D_Map reshade::hooks::call_vtable<10, HRESULT, ID3D10Texture1D, UINT, D3D10_MAP, UINT, void **>
#define ID3D10Texture1D_Unmap reshade::hooks::call_vtable<11, HRESULT, ID3D10Texture1D, UINT>

#define ID3D10Texture2D_Map reshade::hooks::call_vtable<10, HRESULT, ID3D10Texture2D, UINT, D3D10_MAP, UINT, D3D10_MAPPED_TEXTURE2D *>
#define ID3D10Texture2D_Unmap reshade::hooks::call_vtable<11, HRESULT, ID3D10Texture2D, UINT>

#define ID3D10Texture3D_Map reshade::hooks::call_vtable<10, HRESULT, ID3D10Texture3D, UINT, D3D10_MAP, UINT, D3D10_MAPPED_TEXTURE3D *>
#define ID3D10Texture3D_Unmap reshade::hooks::call_vtable<11, HRESULT, ID3D10Texture3D, UINT>

