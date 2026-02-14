/* Libretro D3D12 Hardware Rendering Interface
 *
 * Based on RetroArch's D3D12 HW render interface.
 * When a core requests RETRO_HW_CONTEXT_D3D12 and the frontend uses the
 * d3d12 video driver, the frontend exposes this interface via
 * RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE.
 */

#ifndef LIBRETRO_D3D12_H__
#define LIBRETRO_D3D12_H__

#include "libretro.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <d3d12.h>
#include <dxgi.h>

#define RETRO_HW_RENDER_INTERFACE_D3D12_VERSION 1

struct retro_hw_render_interface_d3d12
{
   enum retro_hw_render_interface_type interface_type;
   unsigned interface_version;

   void *handle;
   ID3D12Device *device;
   ID3D12CommandQueue *queue;

   HRESULT (WINAPI *D3DCompile)(
         LPCVOID pSrcData, SIZE_T SrcDataSize,
         LPCSTR pSourceName,
         const D3D_SHADER_MACRO *pDefines,
         ID3DInclude *pInclude,
         LPCSTR pEntrypoint, LPCSTR pTarget,
         UINT Flags1, UINT Flags2,
         ID3DBlob **ppCode, ID3DBlob **ppErrorMsgs);

   D3D12_RESOURCE_STATES required_state;

   void (*set_texture)(void *handle,
         ID3D12Resource *texture,
         DXGI_FORMAT format);
};

#endif /* LIBRETRO_D3D12_H__ */
