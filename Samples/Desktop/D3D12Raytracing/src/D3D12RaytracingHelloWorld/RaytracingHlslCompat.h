//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#ifdef __cplusplus
#include <DirectXMath.h>
typedef DirectX::XMMATRIX float4x4;
typedef DirectX::XMVECTOR float4;
#endif

struct PerFrameCB
{
    float4x4 projectionToWorld;
    float4x4 viewToWorld;
    float4 cameraPosition;
};

struct PerMaterialCB
{
    float4 diffuse;
    //int normalBufferIdx;
    //int indexBufferIdx;
    //int dummy[2];
};
