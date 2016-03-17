/*     Copyright 2015-2016 Egor Yusov
 *  
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF ANY PROPRIETARY RIGHTS.
 *
 *  In no event and under no legal theory, whether in tort (including negligence), 
 *  contract, or otherwise, unless required by applicable law (such as deliberate 
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental, 
 *  or consequential damages of any character arising as a result of this License or 
 *  out of the use or inability to use the software (including but not limited to damages 
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and 
 *  all other commercial damages or losses), even if such Contributor has been advised 
 *  of the possibility of such damages.
 */

#include "pch.h"

#include "Texture3D_OGL.h"
#include "RenderDeviceGLImpl.h"
#include "DeviceContextGLImpl.h"
#include "GLTypeConversions.h"
#include "GraphicsUtilities.h"

namespace Diligent
{

Texture3D_OGL::Texture3D_OGL( FixedBlockMemoryAllocator& TexObjAllocator, 
                              FixedBlockMemoryAllocator& TexViewObjAllocator,
                              class RenderDeviceGLImpl *pDeviceGL, 
                              class DeviceContextGLImpl *pDeviceContext, 
                              const TextureDesc& TexDesc, 
                              const TextureData &InitData /*= TextureData()*/,
						      bool bIsDeviceInternal /*= false*/) : 
    TextureBaseGL(TexObjAllocator, TexViewObjAllocator, pDeviceGL, TexDesc, InitData, bIsDeviceInternal)
{
    auto &ContextState = pDeviceContext->GetContextState();

    m_BindTarget = GL_TEXTURE_3D;
    ContextState.BindTexture(-1, m_BindTarget, m_GlTexture);
    //                             levels             format          width        height          depth
    glTexStorage3D(m_BindTarget, m_Desc.MipLevels, m_GLTexFormat, m_Desc.Width, m_Desc.Height, m_Desc.Depth);
    CHECK_GL_ERROR_AND_THROW("Failed to allocate storage for the 3D texture");
    // When target is GL_TEXTURE_3D, calling glTexStorage3D is equivalent to the following pseudo-code:
    //for (i = 0; i < levels; i++)
    //{
    //    glTexImage3D(target, i, internalformat, width, height, depth, 0, format, type, NULL);
    //    width = max(1, (width / 2));
    //    height = max(1, (height / 2));
    //    depth = max(1, (depth / 2));
    //

    SetDefaultGLParameters();

    if( InitData.pSubResources )
    {
        if(  m_Desc.MipLevels == InitData.NumSubresources )
        {
            for(Uint32 Mip = 0; Mip < m_Desc.MipLevels; ++Mip)
            {
                Box DstBox(0, std::max(m_Desc.Width >>Mip, 1U),
                            0, std::max(m_Desc.Height>>Mip, 1U), 
                            0, std::max(m_Desc.Depth >>Mip, 1U));
                // UpdateData() is a virtual function. If we try to call it through vtbl from here,
                // we will get into TextureBaseGL::UpdateData(), because instance of Texture3D_OGL
                // is not fully constructed yet.
                // To call the required function, we need to explicitly specify the class: 
                Texture3D_OGL::UpdateData( pDeviceContext, Mip, 0, DstBox, InitData.pSubResources[Mip] );
            }
        }
        else
        {
            UNEXPECTED("Incorrect number of subresources");
        }
    }

    ContextState.BindTexture( -1, m_BindTarget, GLObjectWrappers::GLTextureObj(false) );
}

Texture3D_OGL::~Texture3D_OGL()
{
}


void Texture3D_OGL::UpdateData( IDeviceContext *pContext, Uint32 MipLevel, Uint32 Slice, const Box &DstBox, const TextureSubResData &SubresData )
{
    TextureBaseGL::UpdateData(pContext, MipLevel, Slice, DstBox, SubresData);

    auto *pDeviceContextGL = ValidatedCast<DeviceContextGLImpl>(pContext);
    auto &ContextState = pDeviceContextGL->GetContextState();

    // GL_TEXTURE_UPDATE_BARRIER_BIT: 
    //      Writes to a texture via glTex( Sub )Image*, glCopyTex( Sub )Image*, glClearTex*Image, 
    //      glCompressedTex( Sub )Image*, and reads via glTexImage() after the barrier will reflect 
    //      data written by shaders prior to the barrier. Additionally, texture writes from these 
    //      commands issued after the barrier will not execute until all shader writes initiated prior 
    //      to the barrier complete
    TextureMemoryBarrier( GL_TEXTURE_UPDATE_BARRIER_BIT, ContextState );

    ContextState.BindTexture(-1, m_BindTarget, m_GlTexture);

    // Transfers to OpenGL memory are called unpack operations
    // If there is a buffer bound to GL_PIXEL_UNPACK_BUFFER target, then all the pixel transfer
    // operations will be performed from this buffer. We need to make sure none is bound
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

    auto TransferAttribs = GetNativePixelTransferAttribs(m_Desc.Format);
    
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_IMAGES, 0);

    const auto TexFmtInfo = GetTextureFormatAttribs(m_Desc.Format);
    const auto PixelSize = TexFmtInfo.NumComponents * TexFmtInfo.ComponentSize;
    VERIFY( (SubresData.Stride % PixelSize)==0, "Data stride is not multiple of pixel size" );
    glPixelStorei(GL_UNPACK_ROW_LENGTH, SubresData.Stride / PixelSize);

    VERIFY( (SubresData.DepthStride % SubresData.Stride)==0, "Depth stride is not multiple of stride" );
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, SubresData.DepthStride / SubresData.Stride);

    glTexSubImage3D(m_BindTarget, MipLevel, 
                    DstBox.MinX, 
                    DstBox.MinY,
                    DstBox.MinZ,
                    DstBox.MaxX - DstBox.MinX, 
                    DstBox.MaxY - DstBox.MinY, 
                    DstBox.MaxZ - DstBox.MinZ,
                    TransferAttribs.PixelFormat, TransferAttribs.DataType, 
                    SubresData.pData);
    
    CHECK_GL_ERROR("Failed to update subimage data");

    ContextState.BindTexture( -1, m_BindTarget, GLObjectWrappers::GLTextureObj(false) );
}

void Texture3D_OGL::AttachToFramebuffer( const TextureViewDesc& ViewDesc, GLenum AttachmentPoint )
{
    auto NumDepthSlicesInMip = m_Desc.Depth >> ViewDesc.MostDetailedMip;
    if( ViewDesc.NumDepthSlices == NumDepthSlicesInMip )
    {
        glFramebufferTexture( GL_DRAW_FRAMEBUFFER, AttachmentPoint, m_GlTexture, ViewDesc.MostDetailedMip );
        CHECK_GL_ERROR( "Failed to attach texture 3D to draw framebuffer" );
        glFramebufferTexture( GL_READ_FRAMEBUFFER, AttachmentPoint, m_GlTexture, ViewDesc.MostDetailedMip );
        CHECK_GL_ERROR( "Failed to attach texture 3D to read framebuffer" );
    }
    else if( ViewDesc.NumDepthSlices == 1 )
    {
        // For glFramebufferTexture3D(), if texture name is not zero, then texture target must be GL_TEXTURE_3D
        //glFramebufferTexture3D( GL_DRAW_FRAMEBUFFER, AttachmentPoint, m_BindTarget, m_GlTexture, ViewDesc.MostDetailedMip, ViewDesc.FirstDepthSlice );
        //glFramebufferTexture3D( GL_READ_FRAMEBUFFER, AttachmentPoint, m_BindTarget, m_GlTexture, ViewDesc.MostDetailedMip, ViewDesc.FirstDepthSlice );

        // On Android (at least on Intel HW), glFramebufferTexture3D() runs without errors, but the 
        // FBO turns out to be incomplete. glFramebufferTextureLayer() seems to work fine.
        glFramebufferTextureLayer( GL_DRAW_FRAMEBUFFER, AttachmentPoint, m_GlTexture, ViewDesc.MostDetailedMip, ViewDesc.FirstDepthSlice );
        CHECK_GL_ERROR( "Failed to attach texture 3D to draw framebuffer" );
        glFramebufferTextureLayer( GL_READ_FRAMEBUFFER, AttachmentPoint, m_GlTexture, ViewDesc.MostDetailedMip, ViewDesc.FirstDepthSlice );
        CHECK_GL_ERROR( "Failed to attach texture 3D to read framebuffer" );
    }
    else
    {
        UNEXPECTED( "Only one slice or the entire 3D texture can be attached to a framebuffer" );
    }
}

}
