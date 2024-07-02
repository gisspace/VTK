// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

/**
 * This test fills the color of a texture using a compute shader.
 * A texture filled with 42 grayscale intensity is expected.
 */

#include "TestComputeTextureShader.h"
#include "vtkDataArrayRange.h"
#include "vtkIntArray.h"
#include "vtkNew.h"
#include "vtkWebGPUComputePass.h"
#include "vtkWebGPUComputePipeline.h"
#include "vtkWebGPUComputeTexture.h"
#include "vtkWebGPUTexture.h"

#include <vector>

namespace
{
constexpr int TEXTURE_WIDTH = 160;
constexpr int TEXTURE_HEIGHT = 90;

struct CallbackData
{
  std::vector<unsigned char>* outputData;
  int textureWidth, textureHeight;
};
} // namespace

int TestComputeTexture(int argc, char** argv)
{
  // * 4 for RGBA
  std::vector<unsigned char> referenceOutput(TEXTURE_WIDTH * TEXTURE_HEIGHT * 4);
  for (int y = 0; y < TEXTURE_HEIGHT; y++)
  {
    for (int x = 0; x < TEXTURE_WIDTH; x++)
    {
      int index = x + y * TEXTURE_WIDTH;
      referenceOutput[index * 4 + 0] = 42;
      referenceOutput[index * 4 + 1] = 42;
      referenceOutput[index * 4 + 2] = 42;
      referenceOutput[index * 4 + 3] = 255;
    }
  }

  // Creating the texture that will store the gradient generated by the compute shader
  vtkNew<vtkWebGPUComputeTexture> outputTexture;
  outputTexture->SetLabel("Output texture");
  outputTexture->SetMode(vtkWebGPUComputeTexture::TextureMode::WRITE_ONLY_STORAGE);
  outputTexture->SetFormat(vtkWebGPUComputeTexture::TextureFormat::RGBA8_UNORM);
  outputTexture->SetDimension(vtkWebGPUComputeTexture::TextureDimension::DIMENSION_2D);
  outputTexture->SetSampleType(vtkWebGPUComputeTexture::TextureSampleType::FLOAT);
  outputTexture->SetSize(TEXTURE_WIDTH, TEXTURE_HEIGHT);
  outputTexture->SetDataType(vtkWebGPUComputeTexture::TextureDataType::STD_VECTOR);

  // Creating the compute pipeline
  vtkNew<vtkWebGPUComputePipeline> fillTexturePipeline;

  // Creating the compute pass
  vtkSmartPointer<vtkWebGPUComputePass> fillTextureComputePass =
    fillTexturePipeline->CreateComputePass();
  fillTextureComputePass->SetShaderSource(TestComputeTextureShader);
  fillTextureComputePass->SetShaderEntryPoint("computeColor");
  // Getting the index of the output texture for later mapping with ReadTextureFromGPU()
  int outputTextureIndex = fillTextureComputePass->AddTexture(outputTexture);

  // Creating a write mode texture to be able to write to the texture from the shader
  vtkSmartPointer<vtkWebGPUComputeTextureView> textureView;
  textureView = fillTextureComputePass->CreateTextureView(outputTextureIndex);
  textureView->SetGroup(0);
  textureView->SetBinding(0);
  textureView->SetAspect(vtkWebGPUComputeTextureView::ASPECT_ALL);
  textureView->SetMode(vtkWebGPUComputeTextureView::TextureViewMode::WRITE_ONLY_STORAGE);
  fillTextureComputePass->AddTextureView(textureView);

  // Dispatching the compute with
  int nbXGroups = std::ceil(TEXTURE_WIDTH / 8.0f);
  int nbYGroups = std::ceil(TEXTURE_HEIGHT / 8.0f);
  fillTextureComputePass->SetWorkgroups(nbXGroups, nbYGroups, 1);
  fillTextureComputePass->Dispatch();

  // Output buffer for the result data
  std::vector<unsigned char> outputData(
    TEXTURE_HEIGHT * TEXTURE_WIDTH * outputTexture->GetBytesPerPixel());

  auto onTextureMapped = [](const void* mappedData, int bytesPerRow, void* userdata) {
    CallbackData* data = reinterpret_cast<CallbackData*>(userdata);
    std::vector<unsigned char>* outputData = data->outputData;
    const unsigned char* mappedDataChar = reinterpret_cast<const unsigned char*>(mappedData);

    for (int y = 0; y < data->textureHeight; y++)
    {
      for (int x = 0; x < data->textureWidth; x++)
      {
        int index = x + y * data->textureWidth;
        // Dividing by 4 here because we want to multiply Y by the 'width' which is in number of
        // pixels RGBA, not bytes
        int mappedIndex = x + y * (bytesPerRow / 4);

        // Copying the RGBA channels of each pixel
        (*outputData)[index * 4 + 0] = mappedDataChar[mappedIndex * 4 + 0];
        (*outputData)[index * 4 + 1] = mappedDataChar[mappedIndex * 4 + 1];
        (*outputData)[index * 4 + 2] = mappedDataChar[mappedIndex * 4 + 2];
        (*outputData)[index * 4 + 3] = mappedDataChar[mappedIndex * 4 + 3];
      }
    }
  };

  CallbackData callbackData;
  callbackData.outputData = &outputData;
  callbackData.textureWidth = TEXTURE_WIDTH;
  callbackData.textureHeight = TEXTURE_HEIGHT;

  // Mapping the texture on the CPU to get the results from the GPU
  fillTextureComputePass->ReadTextureFromGPU(outputTextureIndex, 0, onTextureMapped, &callbackData);
  // Update() to actually execute WebGPU commands. Without this, the compute shader won't execute
  // and the data that we try to map here may not be available yet
  fillTexturePipeline->Update();

  for (int i = 0; i < TEXTURE_WIDTH * TEXTURE_HEIGHT; i++)
  {
    // The compute shader is expected to produce the same result as the reference
    // data generated on the CPU
    if (referenceOutput[i] != outputData[i])
    {
      vtkLog(ERROR, "Incorrect result from the mapped texture");

      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}
