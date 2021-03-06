/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2016 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "driver/d3d11/d3d11_resources.h"
#include "3rdparty/lz4/lz4.h"
#include "api/app/renderdoc_app.h"
#include "driver/d3d11/d3d11_context.h"
#include "driver/dxgi/dxgi_wrapped.h"

WRAPPED_POOL_INST(WrappedID3D11Buffer);
WRAPPED_POOL_INST(WrappedID3D11Texture1D);
WRAPPED_POOL_INST(WrappedID3D11Texture2D1);
WRAPPED_POOL_INST(WrappedID3D11Texture3D1);
WRAPPED_POOL_INST(WrappedID3D11InputLayout);
WRAPPED_POOL_INST(WrappedID3D11SamplerState);
WRAPPED_POOL_INST(WrappedID3D11RasterizerState2);
WRAPPED_POOL_INST(WrappedID3D11DepthStencilState);
WRAPPED_POOL_INST(WrappedID3D11BlendState1);
WRAPPED_POOL_INST(WrappedID3D11ShaderResourceView1);
WRAPPED_POOL_INST(WrappedID3D11UnorderedAccessView1);
WRAPPED_POOL_INST(WrappedID3D11RenderTargetView1);
WRAPPED_POOL_INST(WrappedID3D11DepthStencilView);
WRAPPED_POOL_INST(WrappedID3D11Shader<ID3D11VertexShader>);
WRAPPED_POOL_INST(WrappedID3D11Shader<ID3D11HullShader>);
WRAPPED_POOL_INST(WrappedID3D11Shader<ID3D11DomainShader>);
WRAPPED_POOL_INST(WrappedID3D11Shader<ID3D11GeometryShader>);
WRAPPED_POOL_INST(WrappedID3D11Shader<ID3D11PixelShader>);
WRAPPED_POOL_INST(WrappedID3D11Shader<ID3D11ComputeShader>);
WRAPPED_POOL_INST(WrappedID3D11Counter);
WRAPPED_POOL_INST(WrappedID3D11Query1);
WRAPPED_POOL_INST(WrappedID3D11Predicate);
WRAPPED_POOL_INST(WrappedID3D11ClassInstance);
WRAPPED_POOL_INST(WrappedID3D11ClassLinkage);

map<ResourceId, WrappedID3D11Texture1D::TextureEntry>
    WrappedTexture<ID3D11Texture1D, D3D11_TEXTURE1D_DESC, ID3D11Texture1D>::m_TextureList;
map<ResourceId, WrappedID3D11Texture2D1::TextureEntry>
    WrappedTexture<ID3D11Texture2D, D3D11_TEXTURE2D_DESC, ID3D11Texture2D1>::m_TextureList;
map<ResourceId, WrappedID3D11Texture3D1::TextureEntry>
    WrappedTexture<ID3D11Texture3D, D3D11_TEXTURE3D_DESC, ID3D11Texture3D1>::m_TextureList;
map<ResourceId, WrappedID3D11Buffer::BufferEntry> WrappedID3D11Buffer::m_BufferList;
map<ResourceId, WrappedShader::ShaderEntry *> WrappedShader::m_ShaderList;
Threading::CriticalSection WrappedShader::m_ShaderListLock;

const GUID RENDERDOC_ID3D11ShaderGUID_ShaderDebugMagicValue = RENDERDOC_ShaderDebugMagicValue_struct;

void WrappedShader::ShaderEntry::TryReplaceOriginalByteCode()
{
  if(!DXBC::DXBCFile::CheckForDebugInfo((const void *)&m_Bytecode[0], m_Bytecode.size()))
  {
    string originalPath = m_DebugInfoPath;

    if(originalPath.empty())
      originalPath =
          DXBC::DXBCFile::GetDebugBinaryPath((const void *)&m_Bytecode[0], m_Bytecode.size());

    if(!originalPath.empty())
    {
      bool lz4 = false;

      if(!strncmp(originalPath.c_str(), "lz4#", 4))
      {
        originalPath = originalPath.substr(4);
        lz4 = true;
      }
      // could support more if we're willing to compile in the decompressor

      FILE *originalShaderFile = NULL;

      size_t numSearchPaths = m_DebugInfoSearchPaths ? m_DebugInfoSearchPaths->size() : 0;

      string foundPath;

      // while we haven't found a file, keep trying through the search paths. For i==0
      // check the path on its own, in case it's an absolute path.
      for(size_t i = 0; originalShaderFile == NULL && i <= numSearchPaths; i++)
      {
        if(i == 0)
        {
          originalShaderFile = FileIO::fopen(originalPath.c_str(), "rb");
          foundPath = originalPath;
          continue;
        }
        else
        {
          const std::string &searchPath = (*m_DebugInfoSearchPaths)[i - 1];
          foundPath = searchPath + "/" + originalPath;
          originalShaderFile = FileIO::fopen(foundPath.c_str(), "rb");
        }
      }

      if(originalShaderFile == NULL)
        return;

      FileIO::fseek64(originalShaderFile, 0L, SEEK_END);
      uint64_t originalShaderSize = FileIO::ftell64(originalShaderFile);
      FileIO::fseek64(originalShaderFile, 0, SEEK_SET);

      if(lz4 || originalShaderSize >= m_Bytecode.size())
      {
        vector<byte> originalBytecode;

        originalBytecode.resize((size_t)originalShaderSize);
        FileIO::fread(&originalBytecode[0], sizeof(byte), (size_t)originalShaderSize,
                      originalShaderFile);

        if(lz4)
        {
          vector<byte> decompressed;

          // first try decompressing to 1MB flat
          decompressed.resize(100 * 1024);

          int ret = LZ4_decompress_safe((const char *)&originalBytecode[0], (char *)&decompressed[0],
                                        (int)originalBytecode.size(), (int)decompressed.size());

          if(ret < 0)
          {
            // if it failed, either source is corrupt or we didn't allocate enough space.
            // Just allocate 255x compressed size since it can't need any more than that.
            decompressed.resize(255 * originalBytecode.size());

            ret = LZ4_decompress_safe((const char *)&originalBytecode[0], (char *)&decompressed[0],
                                      (int)originalBytecode.size(), (int)decompressed.size());

            if(ret < 0)
            {
              RDCERR("Failed to decompress LZ4 data from %s", foundPath.c_str());
              return;
            }
          }

          RDCASSERT(ret > 0, ret);

          // we resize and memcpy instead of just doing .swap() because that would
          // transfer over the over-large pessimistic capacity needed for decompression
          originalBytecode.resize(ret);
          memcpy(&originalBytecode[0], &decompressed[0], originalBytecode.size());
        }

        if(DXBC::DXBCFile::CheckForDebugInfo((const void *)&originalBytecode[0],
                                             originalBytecode.size()))
        {
          m_Bytecode.swap(originalBytecode);
        }
      }

      FileIO::fclose(originalShaderFile);
    }
  }
}

UINT GetMipForSubresource(ID3D11Resource *res, int Subresource)
{
  D3D11_RESOURCE_DIMENSION dim;

  // check for wrapped types first as they will be most common and don't
  // require a virtual call
  if(WrappedID3D11Texture1D::IsAlloc(res))
    dim = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
  else if(WrappedID3D11Texture2D1::IsAlloc(res))
    dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
  else if(WrappedID3D11Texture3D1::IsAlloc(res))
    dim = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
  else
    res->GetType(&dim);

  ID3D11Texture1D *tex1 = (dim == D3D11_RESOURCE_DIMENSION_TEXTURE1D) ? (ID3D11Texture1D *)res : NULL;
  ID3D11Texture2D *tex2 = (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ? (ID3D11Texture2D *)res : NULL;
  ID3D11Texture3D *tex3 = (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) ? (ID3D11Texture3D *)res : NULL;

  RDCASSERT(tex1 || tex2 || tex3);

  UINT mipLevel = Subresource;

  if(tex1)
  {
    D3D11_TEXTURE1D_DESC desc;
    tex1->GetDesc(&desc);

    int mipLevels = desc.MipLevels;

    if(mipLevels == 0)
      mipLevels = CalcNumMips(desc.Width, 1, 1);

    mipLevel %= mipLevels;
  }
  else if(tex2)
  {
    D3D11_TEXTURE2D_DESC desc;
    tex2->GetDesc(&desc);

    int mipLevels = desc.MipLevels;

    if(mipLevels == 0)
      mipLevels = CalcNumMips(desc.Width, desc.Height, 1);

    mipLevel %= mipLevels;
  }
  else if(tex3)
  {
    D3D11_TEXTURE3D_DESC desc;
    tex3->GetDesc(&desc);

    int mipLevels = desc.MipLevels;

    if(mipLevels == 0)
      mipLevels = CalcNumMips(desc.Width, desc.Height, desc.Depth);

    mipLevel %= mipLevels;
  }

  return mipLevel;
}

UINT GetByteSize(ID3D11Texture1D *tex, int SubResource)
{
  D3D11_TEXTURE1D_DESC desc;
  tex->GetDesc(&desc);

  return GetByteSize(desc.Width, 1, 1, desc.Format, SubResource % desc.MipLevels);
}

UINT GetByteSize(ID3D11Texture2D *tex, int SubResource)
{
  D3D11_TEXTURE2D_DESC desc;
  tex->GetDesc(&desc);

  return GetByteSize(desc.Width, desc.Height, 1, desc.Format, SubResource % desc.MipLevels);
}

UINT GetByteSize(ID3D11Texture3D *tex, int SubResource)
{
  D3D11_TEXTURE3D_DESC desc;
  tex->GetDesc(&desc);

  return GetByteSize(desc.Width, desc.Height, desc.Depth, desc.Format, SubResource);
}

UINT GetFormatBPP(DXGI_FORMAT f)
{
  UINT ret = 8;

  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT: ret *= 16; break;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT: ret *= 12; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: ret *= 8; break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_R8G8_B8G8_UNORM:
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: ret *= 4; break;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM: ret *= 2; break;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM: ret *= 1; break;
    case DXGI_FORMAT_R1_UNORM: ret /= 8; break;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
      // return block size (in bits)
      ret *= 8;
      break;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      // return block size (in bits)
      ret *= 16;
      break;

    case DXGI_FORMAT_AYUV:
    case DXGI_FORMAT_Y410:
    case DXGI_FORMAT_YUY2:
    case DXGI_FORMAT_Y416:
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
    case DXGI_FORMAT_420_OPAQUE:
    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216:
    case DXGI_FORMAT_NV11:
    case DXGI_FORMAT_AI44:
    case DXGI_FORMAT_IA44:
    case DXGI_FORMAT_P8:
    case DXGI_FORMAT_A8P8:
    case DXGI_FORMAT_P208:
    case DXGI_FORMAT_V208:
    case DXGI_FORMAT_V408: RDCERR("Video formats not supported"); break;

    case DXGI_FORMAT_B4G4R4A4_UNORM:
      ret *= 2;    // 4 channels, half a byte each
      break;

    case DXGI_FORMAT_UNKNOWN:
      ret = 0;
      RDCWARN("Getting BPP of DXGI_FORMAT_UNKNOWN");
      break;

    default: RDCERR("Unrecognised DXGI Format: %d", f); break;
  }

  return ret;
}

UINT GetByteSize(int Width, int Height, int Depth, DXGI_FORMAT Format, int mip)
{
  UINT ret = RDCMAX(Width >> mip, 1) * RDCMAX(Height >> mip, 1) * RDCMAX(Depth >> mip, 1);

  switch(Format)
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT: ret *= 16; break;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT: ret *= 12; break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: ret *= 8; break;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_R8G8_B8G8_UNORM:
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: ret *= 4; break;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM: ret *= 2; break;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM: ret *= 1; break;
    case DXGI_FORMAT_R1_UNORM: ret = RDCMAX(ret / 8, 1U); break;
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
      ret = AlignUp4(RDCMAX(Width >> mip, 1)) * AlignUp4(RDCMAX(Height >> mip, 1)) *
            RDCMAX(Depth >> mip, 1);
      ret /= 2;
      break;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
      ret = AlignUp4(RDCMAX(Width >> mip, 1)) * AlignUp4(RDCMAX(Height >> mip, 1)) *
            RDCMAX(Depth >> mip, 1);
      ret *= 1;
      break;
    case DXGI_FORMAT_AYUV:
    case DXGI_FORMAT_Y410:
    case DXGI_FORMAT_YUY2:
    case DXGI_FORMAT_Y416:
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
    case DXGI_FORMAT_420_OPAQUE:
    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216:
    case DXGI_FORMAT_NV11:
    case DXGI_FORMAT_AI44:
    case DXGI_FORMAT_IA44:
    case DXGI_FORMAT_P8:
    case DXGI_FORMAT_A8P8:
    case DXGI_FORMAT_P208:
    case DXGI_FORMAT_V208:
    case DXGI_FORMAT_V408: RDCERR("Video formats not supported"); break;

    case DXGI_FORMAT_B4G4R4A4_UNORM:
      ret *= 2;    // 4 channels, half a byte each
      break;
    case DXGI_FORMAT_UNKNOWN:
      RDCERR("Getting byte size of unknown DXGI format");
      ret = 0;
      break;
    default: RDCERR("Unrecognised DXGI Format: %d", Format); break;
  }

  return ret;
}

bool IsBlockFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB: return true;
    default: break;
  }

  return false;
}

bool IsDepthFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:

    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:

    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D16_UNORM: return true;
  }

  return false;
}

bool IsTypelessFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS: return true;
  }

  return false;
}

bool IsUIntFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R8_UINT: return true;
  }

  return false;
}

bool IsIntFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_SINT:
    case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8_SINT: return true;
  }

  return false;
}

bool IsSRGBFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return true;

    default: break;
  }

  return false;
}

DXGI_FORMAT GetDepthTypedFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;

    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;

    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return DXGI_FORMAT_D24_UNORM_S8_UINT;

    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;

    default: break;
  }
  return f;
}

DXGI_FORMAT GetNonSRGBFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;

    default: break;
  }

  return f;
}

DXGI_FORMAT GetSRGBFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM: return DXGI_FORMAT_BC1_UNORM_SRGB;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM: return DXGI_FORMAT_BC2_UNORM_SRGB;

    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM: return DXGI_FORMAT_BC3_UNORM_SRGB;

    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM: return DXGI_FORMAT_BC7_UNORM_SRGB;

    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;

    default: break;
  }
  return f;
}

DXGI_FORMAT GetUnormTypedFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_UNORM;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UINT: return DXGI_FORMAT_R10G10B10A2_UNORM;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_UNORM;

    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_UNORM;

    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT: return DXGI_FORMAT_R8G8_UNORM;

    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT: return DXGI_FORMAT_R16_UNORM;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT: return DXGI_FORMAT_R8_UNORM;

    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;

    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;

    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_SNORM: return DXGI_FORMAT_BC4_UNORM;

    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_SNORM: return DXGI_FORMAT_BC5_UNORM;

    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;

    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;

    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_SF16: return DXGI_FORMAT_BC6H_UF16;

    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;

    default: break;
  }

  return f;
}

DXGI_FORMAT GetSnormTypedFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_SNORM;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_SNORM;

    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_SNORM;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT: return DXGI_FORMAT_R8G8_SNORM;

    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SINT: return DXGI_FORMAT_R16_SNORM;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM: return DXGI_FORMAT_R8_SNORM;

    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM: return DXGI_FORMAT_BC4_SNORM;

    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM: return DXGI_FORMAT_BC5_SNORM;

    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16: return DXGI_FORMAT_BC6H_SF16;

    default: break;
  }

  return f;
}

DXGI_FORMAT GetUIntTypedFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_SINT: return DXGI_FORMAT_R32G32B32A32_UINT;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_SINT: return DXGI_FORMAT_R32G32B32_UINT;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_UINT;

    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_SINT: return DXGI_FORMAT_R32G32_UINT;

    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UINT;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_UINT;

    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_UINT;

    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_SINT: return DXGI_FORMAT_R32_UINT;

    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return DXGI_FORMAT_X24_TYPELESS_G8_UINT;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT: return DXGI_FORMAT_R8G8_UINT;

    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT: return DXGI_FORMAT_R16_UINT;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM: return DXGI_FORMAT_R8_UINT;

    default: break;
  }

  return f;
}

DXGI_FORMAT GetSIntTypedFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_SINT;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT: return DXGI_FORMAT_R32G32B32_SINT;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: return DXGI_FORMAT_R16G16B16A16_SINT;

    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT: return DXGI_FORMAT_R32G32_SINT;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SINT;

    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM: return DXGI_FORMAT_R16G16_SINT;

    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT: return DXGI_FORMAT_R32_SINT;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM: return DXGI_FORMAT_R8G8_SINT;

    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM: return DXGI_FORMAT_R16_SINT;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM: return DXGI_FORMAT_R8_SINT;

    default: break;
  }

  return f;
}

DXGI_FORMAT GetFloatTypedFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_SINT:
    case DXGI_FORMAT_R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_FLOAT;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R32G32B32_UINT: return DXGI_FORMAT_R32G32B32_FLOAT;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R16G16B16A16_UINT: return DXGI_FORMAT_R16G16B16A16_FLOAT;

    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G32_UINT: return DXGI_FORMAT_R32G32_FLOAT;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UINT: return DXGI_FORMAT_R10G10B10A2_UNORM;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;

    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM: return DXGI_FORMAT_R16G16_FLOAT;

    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT: return DXGI_FORMAT_R32_FLOAT;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM: return DXGI_FORMAT_R8G8_UNORM;

    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM: return DXGI_FORMAT_R16_FLOAT;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM: return DXGI_FORMAT_R8_UNORM;
  }

  return GetTypedFormat(f);
}

DXGI_FORMAT GetTypedFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;

    case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;

    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;

    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;

    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;

    case DXGI_FORMAT_R32_TYPELESS:
      return DXGI_FORMAT_R32_FLOAT;

    // maybe not valid casts?
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;

    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;

    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;

    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;

    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;

    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;

    case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;

    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;

    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;

    case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;

    case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;

    case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;

    default: break;
  }

  return f;
}

DXGI_FORMAT GetTypelessFormat(DXGI_FORMAT f)
{
  switch(f)
  {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT: return DXGI_FORMAT_R32G32B32A32_TYPELESS;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT: return DXGI_FORMAT_R32G32B32_TYPELESS;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;

    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT: return DXGI_FORMAT_R32G32_TYPELESS;

    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:    // maybe not valid cast?
      return DXGI_FORMAT_R10G10B10A2_TYPELESS;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT: return DXGI_FORMAT_R8G8B8A8_TYPELESS;

    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_TYPELESS;

    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:    // maybe not valid cast?
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
      return DXGI_FORMAT_R32_TYPELESS;

    // maybe not valid casts?
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;

    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8_B8G8_UNORM:    // maybe not valid cast?
    case DXGI_FORMAT_G8R8_G8B8_UNORM:    // maybe not valid cast?
      return DXGI_FORMAT_B8G8R8A8_TYPELESS;

    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;

    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT: return DXGI_FORMAT_R8G8_TYPELESS;

    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT: return DXGI_FORMAT_R16_TYPELESS;

    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM: return DXGI_FORMAT_R8_TYPELESS;

    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_TYPELESS;

    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM: return DXGI_FORMAT_BC4_TYPELESS;

    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_TYPELESS;

    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_TYPELESS;

    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM: return DXGI_FORMAT_BC5_TYPELESS;

    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16: return DXGI_FORMAT_BC6H_TYPELESS;

    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_TYPELESS;

    case DXGI_FORMAT_R1_UNORM:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_AYUV:
    case DXGI_FORMAT_Y410:
    case DXGI_FORMAT_YUY2:
    case DXGI_FORMAT_Y416:
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
    case DXGI_FORMAT_420_OPAQUE:
    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216:
    case DXGI_FORMAT_NV11:
    case DXGI_FORMAT_AI44:
    case DXGI_FORMAT_IA44:
    case DXGI_FORMAT_P8:
    case DXGI_FORMAT_A8P8:
    case DXGI_FORMAT_P208:
    case DXGI_FORMAT_V208:
    case DXGI_FORMAT_V408:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
      RDCERR("No Typeless DXGI Format for %d", f);
      return DXGI_FORMAT_UNKNOWN;

    case DXGI_FORMAT_UNKNOWN:
      RDCWARN("Getting Typeless format of DXGI_FORMAT_UNKNOWN");
      return DXGI_FORMAT_UNKNOWN;

    default: RDCERR("Unrecognised DXGI Format: %d", f); return DXGI_FORMAT_UNKNOWN;
  }
}

string ToStrHelper<false, ResourceType>::Get(const ResourceType &el)
{
  switch(el)
  {
    TOSTR_CASE_STRINGIZE(Resource_InputLayout)
    TOSTR_CASE_STRINGIZE(Resource_Buffer)
    TOSTR_CASE_STRINGIZE(Resource_Texture1D)
    TOSTR_CASE_STRINGIZE(Resource_Texture2D)
    TOSTR_CASE_STRINGIZE(Resource_Texture3D)
    TOSTR_CASE_STRINGIZE(Resource_RasterizerState)
    TOSTR_CASE_STRINGIZE(Resource_BlendState)
    TOSTR_CASE_STRINGIZE(Resource_DepthStencilState)
    TOSTR_CASE_STRINGIZE(Resource_SamplerState)
    TOSTR_CASE_STRINGIZE(Resource_RenderTargetView)
    TOSTR_CASE_STRINGIZE(Resource_ShaderResourceView)
    TOSTR_CASE_STRINGIZE(Resource_DepthStencilView)
    TOSTR_CASE_STRINGIZE(Resource_Shader)
    TOSTR_CASE_STRINGIZE(Resource_UnorderedAccessView)
    TOSTR_CASE_STRINGIZE(Resource_Counter)
    TOSTR_CASE_STRINGIZE(Resource_Query)
    TOSTR_CASE_STRINGIZE(Resource_Predicate)
    TOSTR_CASE_STRINGIZE(Resource_ClassInstance)
    TOSTR_CASE_STRINGIZE(Resource_ClassLinkage)

    TOSTR_CASE_STRINGIZE(Resource_DeviceContext)
    TOSTR_CASE_STRINGIZE(Resource_CommandList)
    default: break;
  }

  char tostrBuf[256] = {0};
  StringFormat::snprintf(tostrBuf, 255, "ResourceType<%d>", el);

  return tostrBuf;
}

ResourceId GetIDForResource(ID3D11DeviceChild *ptr)
{
  if(ptr == NULL)
    return ResourceId();

  if(WrappedID3D11InputLayout::IsAlloc(ptr))
    return ((WrappedID3D11InputLayout *)ptr)->GetResourceID();

  if(WrappedID3D11Shader<ID3D11VertexShader>::IsAlloc(ptr))
    return ((WrappedID3D11Shader<ID3D11VertexShader> *)ptr)->GetResourceID();
  if(WrappedID3D11Shader<ID3D11PixelShader>::IsAlloc(ptr))
    return ((WrappedID3D11Shader<ID3D11PixelShader> *)ptr)->GetResourceID();
  if(WrappedID3D11Shader<ID3D11GeometryShader>::IsAlloc(ptr))
    return ((WrappedID3D11Shader<ID3D11GeometryShader> *)ptr)->GetResourceID();
  if(WrappedID3D11Shader<ID3D11HullShader>::IsAlloc(ptr))
    return ((WrappedID3D11Shader<ID3D11HullShader> *)ptr)->GetResourceID();
  if(WrappedID3D11Shader<ID3D11DomainShader>::IsAlloc(ptr))
    return ((WrappedID3D11Shader<ID3D11DomainShader> *)ptr)->GetResourceID();
  if(WrappedID3D11Shader<ID3D11ComputeShader>::IsAlloc(ptr))
    return ((WrappedID3D11Shader<ID3D11ComputeShader> *)ptr)->GetResourceID();

  if(WrappedID3D11Buffer::IsAlloc(ptr))
    return ((WrappedID3D11Buffer *)ptr)->GetResourceID();

  if(WrappedID3D11Texture1D::IsAlloc(ptr))
    return ((WrappedID3D11Texture1D *)ptr)->GetResourceID();
  if(WrappedID3D11Texture2D1::IsAlloc(ptr))
    return ((WrappedID3D11Texture2D1 *)ptr)->GetResourceID();
  if(WrappedID3D11Texture3D1::IsAlloc(ptr))
    return ((WrappedID3D11Texture3D1 *)ptr)->GetResourceID();

  if(WrappedID3D11RasterizerState2::IsAlloc(ptr))
    return ((WrappedID3D11RasterizerState2 *)ptr)->GetResourceID();
  if(WrappedID3D11BlendState1::IsAlloc(ptr))
    return ((WrappedID3D11BlendState1 *)ptr)->GetResourceID();
  if(WrappedID3D11DepthStencilState::IsAlloc(ptr))
    return ((WrappedID3D11DepthStencilState *)ptr)->GetResourceID();
  if(WrappedID3D11SamplerState::IsAlloc(ptr))
    return ((WrappedID3D11SamplerState *)ptr)->GetResourceID();

  if(WrappedID3D11RenderTargetView1::IsAlloc(ptr))
    return ((WrappedID3D11RenderTargetView1 *)ptr)->GetResourceID();
  if(WrappedID3D11ShaderResourceView1::IsAlloc(ptr))
    return ((WrappedID3D11ShaderResourceView1 *)ptr)->GetResourceID();
  if(WrappedID3D11DepthStencilView::IsAlloc(ptr))
    return ((WrappedID3D11DepthStencilView *)ptr)->GetResourceID();
  if(WrappedID3D11UnorderedAccessView1::IsAlloc(ptr))
    return ((WrappedID3D11UnorderedAccessView1 *)ptr)->GetResourceID();

  if(WrappedID3D11Counter::IsAlloc(ptr))
    return ((WrappedID3D11Counter *)ptr)->GetResourceID();
  if(WrappedID3D11Query1::IsAlloc(ptr))
    return ((WrappedID3D11Query1 *)ptr)->GetResourceID();
  if(WrappedID3D11Predicate::IsAlloc(ptr))
    return ((WrappedID3D11Predicate *)ptr)->GetResourceID();

  if(WrappedID3D11ClassInstance::IsAlloc(ptr))
    return ((WrappedID3D11ClassInstance *)ptr)->GetResourceID();
  if(WrappedID3D11ClassLinkage::IsAlloc(ptr))
    return ((WrappedID3D11ClassLinkage *)ptr)->GetResourceID();

  if(WrappedID3D11DeviceContext::IsAlloc(ptr))
    return ((WrappedID3D11DeviceContext *)ptr)->GetResourceID();
  if(WrappedID3D11CommandList::IsAlloc(ptr))
    return ((WrappedID3D11CommandList *)ptr)->GetResourceID();

  RDCERR("Unknown type for ptr 0x%p", ptr);

  return ResourceId();
}

ResourceType IdentifyTypeByPtr(IUnknown *ptr)
{
  if(WrappedID3D11InputLayout::IsAlloc(ptr))
    return Resource_InputLayout;

  if(WrappedID3D11Shader<ID3D11VertexShader>::IsAlloc(ptr) ||
     WrappedID3D11Shader<ID3D11PixelShader>::IsAlloc(ptr) ||
     WrappedID3D11Shader<ID3D11GeometryShader>::IsAlloc(ptr) ||
     WrappedID3D11Shader<ID3D11HullShader>::IsAlloc(ptr) ||
     WrappedID3D11Shader<ID3D11DomainShader>::IsAlloc(ptr) ||
     WrappedID3D11Shader<ID3D11ComputeShader>::IsAlloc(ptr))
    return Resource_Shader;

  if(WrappedID3D11Buffer::IsAlloc(ptr))
    return Resource_Buffer;

  if(WrappedID3D11Texture1D::IsAlloc(ptr))
    return Resource_Texture1D;
  if(WrappedID3D11Texture2D1::IsAlloc(ptr))
    return Resource_Texture2D;
  if(WrappedID3D11Texture3D1::IsAlloc(ptr))
    return Resource_Texture3D;

  if(WrappedID3D11RasterizerState2::IsAlloc(ptr))
    return Resource_RasterizerState;
  if(WrappedID3D11BlendState1::IsAlloc(ptr))
    return Resource_BlendState;
  if(WrappedID3D11DepthStencilState::IsAlloc(ptr))
    return Resource_DepthStencilState;
  if(WrappedID3D11SamplerState::IsAlloc(ptr))
    return Resource_SamplerState;

  if(WrappedID3D11RenderTargetView1::IsAlloc(ptr))
    return Resource_RenderTargetView;
  if(WrappedID3D11ShaderResourceView1::IsAlloc(ptr))
    return Resource_ShaderResourceView;
  if(WrappedID3D11DepthStencilView::IsAlloc(ptr))
    return Resource_DepthStencilView;
  if(WrappedID3D11UnorderedAccessView1::IsAlloc(ptr))
    return Resource_UnorderedAccessView;

  if(WrappedID3D11Counter::IsAlloc(ptr))
    return Resource_Counter;
  if(WrappedID3D11Query1::IsAlloc(ptr))
    return Resource_Query;
  if(WrappedID3D11Predicate::IsAlloc(ptr))
    return Resource_Predicate;

  if(WrappedID3D11ClassInstance::IsAlloc(ptr))
    return Resource_ClassInstance;
  if(WrappedID3D11ClassLinkage::IsAlloc(ptr))
    return Resource_ClassLinkage;

  if(WrappedID3D11DeviceContext::IsAlloc(ptr))
    return Resource_DeviceContext;
  if(WrappedID3D11CommandList::IsAlloc(ptr))
    return Resource_CommandList;

  RDCERR("Unknown type for ptr 0x%p", ptr);

  return Resource_Unknown;
}

HRESULT STDMETHODCALLTYPE RefCounter::QueryInterface(
    /* [in] */ REFIID riid,
    /* [annotation][iid_is][out] */
    __RPC__deref_out void **ppvObject)
{
  return RefCountDXGIObject::WrapQueryInterface(m_pReal, riid, ppvObject);
}

unsigned int RefCounter::SoftRef(WrappedID3D11Device *device)
{
  unsigned int ret = AddRef();
  if(device)
    device->SoftRef();
  else
    RDCWARN("No device pointer, is a deleted resource being AddRef()d?");
  return ret;
}

unsigned int RefCounter::SoftRelease(WrappedID3D11Device *device)
{
  unsigned int ret = Release();
  if(device)
    device->SoftRelease();
  else
    RDCWARN("No device pointer, is a deleted resource being Release()d?");
  return ret;
}

void RefCounter::AddDeviceSoftref(WrappedID3D11Device *device)
{
  if(device)
    device->SoftRef();
}

void RefCounter::ReleaseDeviceSoftref(WrappedID3D11Device *device)
{
  if(device)
    device->SoftRelease();
}
