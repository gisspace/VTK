/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkOpenGLES30PolyDataMapper.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkOpenGLES30PolyDataMapper.h"
#include "vtkActor.h"
#include "vtkArrayDispatch.h"
#include "vtkCellData.h"
#include "vtkDataArray.h"
#include "vtkDataArrayRange.h"
#include "vtkFloatArray.h"
#include "vtkHardwareSelector.h"
#include "vtkImageData.h"
#include "vtkLightCollection.h"
#include "vtkLogger.h"
#include "vtkOpenGLCellToVTKCellMap.h"
#include "vtkOpenGLError.h"
#include "vtkOpenGLIndexBufferObject.h"
#include "vtkOpenGLRenderTimer.h"
#include "vtkOpenGLRenderWindow.h"
#include "vtkOpenGLRenderer.h"
#include "vtkOpenGLResourceFreeCallback.h"
#include "vtkOpenGLState.h"
#include "vtkOpenGLTexture.h"
#include "vtkOpenGLVertexArrayObject.h"
#include "vtkOpenGLVertexBufferObject.h"
#include "vtkOpenGLVertexBufferObjectGroup.h"
#include "vtkPointData.h"
#include "vtkPolyData.h"
#include "vtkPolyDataVS.h"
#include "vtkProperty.h"
#include "vtkRenderer.h"
#include "vtkShaderProgram.h"
#include "vtkShaderProperty.h"
#include "vtkTransform.h"
#include "vtk_glew.h"

VTK_ABI_NAMESPACE_BEGIN

namespace
{

template <typename T>
class ScopedValueRollback
{
public:
  ScopedValueRollback(T& value, T newValue)
  {
    Value = value;
    Pointer = &value;
    *Pointer = newValue;
  }
  ~ScopedValueRollback() { *Pointer = Value; }

private:
  T* Pointer = nullptr;
  T Value;
};

// helper to get the state of picking
int getPickState(vtkRenderer* ren)
{
  vtkHardwareSelector* selector = ren->GetSelector();
  if (selector)
  {
    return selector->GetCurrentPass();
  }

  return vtkHardwareSelector::MIN_KNOWN_PASS - 1;
}

struct VertexAttributeArrays
{
  vtkSmartPointer<vtkDataArray> colors;
  vtkSmartPointer<vtkDataArray> normals;
  vtkSmartPointer<vtkDataArray> points;
  vtkSmartPointer<vtkDataArray> tangents;
  vtkSmartPointer<vtkDataArray> tcoords;

  void operator=(VertexAttributeArrays& other)
  {
    if (other.colors != nullptr)
    {
      this->colors = vtk::TakeSmartPointer(other.colors->NewInstance());
      this->colors->SetNumberOfComponents(other.colors->GetNumberOfComponents());
    }
    if (other.normals != nullptr)
    {
      this->normals = vtk::TakeSmartPointer(other.normals->NewInstance());
      this->normals->SetNumberOfComponents(other.normals->GetNumberOfComponents());
    }
    if (other.points != nullptr)
    {
      this->points = vtk::TakeSmartPointer(other.points->NewInstance());
      this->points->SetNumberOfComponents(other.points->GetNumberOfComponents());
    }
    if (other.tangents != nullptr)
    {
      this->tangents = vtk::TakeSmartPointer(other.tangents->NewInstance());
      this->tangents->SetNumberOfComponents(other.tangents->GetNumberOfComponents());
    }
    if (other.tcoords != nullptr)
    {
      this->tcoords = vtk::TakeSmartPointer(other.tcoords->NewInstance());
      this->tcoords->SetNumberOfComponents(other.tcoords->GetNumberOfComponents());
    }
  }

  void Resize(int npts)
  {
    if (this->colors != nullptr)
    {
      this->colors->SetNumberOfTuples(npts);
    }
    if (this->normals != nullptr)
    {
      this->normals->SetNumberOfTuples(npts);
    }
    if (this->points != nullptr)
    {
      this->points->SetNumberOfTuples(npts);
    }
    if (this->tangents != nullptr)
    {
      this->tangents->SetNumberOfTuples(npts);
    }
    if (this->tcoords != nullptr)
    {
      this->tcoords->SetNumberOfTuples(npts);
    }
  }
};

struct vtkExpandVertexAttributes
{
  template <typename T1, typename T2>
  void operator()(T1* src, T2* dst, const unsigned int* indices, const std::size_t numIndices)
  {
    auto srcRange = vtk::DataArrayTupleRange(src);
    auto dstRange = vtk::DataArrayTupleRange(dst);
    const int numComponents = src->GetNumberOfComponents();
    if (numComponents != dst->GetNumberOfComponents())
    {
      vtkLog(ERROR, << __func__ << ": Mismatch in source and destination components.");
    }
    int dstPtId = 0;
    for (std::size_t i = 0; i < numIndices; ++i)
    {
      const auto& ptId = indices[i];
      for (int comp = 0; comp < numComponents; ++comp)
      {
        dstRange[dstPtId][comp] = srcRange[ptId][comp];
      }
      ++dstPtId;
    }
  }
};

struct vtkPopulateNeighborVertices
{
  template <typename T1, typename T2, typename T3>
  void operator()(T1* input, T2* prev, T3* next, const vtkIdType primitiveSize)
  {
    auto inputRange = vtk::DataArrayValueRange(input);
    auto nextRange = vtk::DataArrayValueRange(next);
    auto prevRange = vtk::DataArrayValueRange(prev);
    const int numComponents = input->GetNumberOfComponents();
    if (numComponents != prev->GetNumberOfComponents() ||
      numComponents != next->GetNumberOfComponents())
    {
      vtkLog(ERROR, << __func__ << ": Mismatch in input and prev,next number of components.");
    }
    for (vtkIdType i = 0; i < input->GetNumberOfValues(); i += primitiveSize * numComponents)
    {
      auto start = inputRange.begin() + i;
      auto nextStart = nextRange.begin() + i;
      std::rotate_copy(
        start, start + numComponents, start + primitiveSize * numComponents, nextStart);
      auto prevStart = prevRange.begin() + i;
      std::rotate_copy(start, start + primitiveSize * numComponents - numComponents,
        start + primitiveSize * numComponents, prevStart);
    }
  }
};

} // end anonymous namespace

//------------------------------------------------------------------------------
vtkStandardNewMacro(vtkOpenGLES30PolyDataMapper);

//------------------------------------------------------------------------------
vtkOpenGLES30PolyDataMapper::vtkOpenGLES30PolyDataMapper() = default;

//------------------------------------------------------------------------------
vtkOpenGLES30PolyDataMapper::~vtkOpenGLES30PolyDataMapper() = default;

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::PrintSelf(ostream& os, vtkIndent indent) {}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::RenderPieceStart(vtkRenderer* ren, vtkActor* act)
{

  // timer calls take time, for lots of "small" actors
  // the timer can be a big hit. So we only update
  // once per million cells or every 100 renders
  // whichever happens first
  vtkIdType numCells = this->CurrentInput->GetNumberOfCells();
  if (numCells != 0)
  {
    this->TimerQueryCounter++;
    if (this->TimerQueryCounter > 100 ||
      static_cast<double>(this->TimerQueryCounter) > 1000000.0 / numCells)
    {
      this->TimerQuery->ReusableStart();
      this->TimerQueryCounter = 0;
    }
  }

  int picking = getPickState(ren);
  if (this->LastSelectionState != picking)
  {
    this->SelectionStateChanged.Modified();
    this->LastSelectionState = picking;
  }

  this->PrimitiveIDOffset = 0;

  // make sure the BOs are up to date
  this->UpdateBufferObjects(ren, act);

  // If we are coloring by texture, then load the texture map.
  // Use Map as indicator, because texture hangs around.
  if (this->ColorTextureMap)
  {
    this->InternalColorTexture->Load(ren);
  }

  this->LastBoundBO = nullptr;
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::RenderPieceDraw(vtkRenderer* ren, vtkActor* act)
{
  const int representation = act->GetProperty()->GetRepresentation();
  vtkOpenGLRenderWindow* renWin = static_cast<vtkOpenGLRenderWindow*>(ren->GetRenderWindow());
  vtkOpenGLState* ostate = renWin->GetState();
  vtkHardwareSelector* selector = ren->GetSelector();
  bool draw_surface_with_edges =
    (act->GetProperty()->GetEdgeVisibility() && representation == VTK_SURFACE) && !selector;

  for (int primType = PrimitiveStart;
       primType < (draw_surface_with_edges ? PrimitiveEnd : PrimitiveTriStrips + 1); ++primType)
  {
    this->DrawingVertices = primType > PrimitiveTriStrips;
    if (this->PrimitiveIndexArrays[primType].empty())
    {
      continue;
    }
    const auto numVerts = this->PrimitiveIndexArrays[primType].size();
    ScopedValueRollback<vtkOpenGLVertexBufferObjectGroup*> vbogBkp(
      this->VBOs, this->PrimitiveVBOGroup[primType].Get());
    ScopedValueRollback<std::size_t> indexCountBkp(
      this->Primitives[primType].IBO->IndexCount, numVerts);

    this->UpdateShaders(this->Primitives[primType], ren, act);
    GLenum mode = this->PointPicking ? GL_POINTS : this->GetOpenGLMode(representation, primType);
    glDrawArrays(mode, 0, numVerts);
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::RenderPieceFinish(vtkRenderer* ren, vtkActor* act)
{
  if (this->LastBoundBO)
  {
    this->LastBoundBO->VAO->Release();
  }

  if (this->ColorTextureMap)
  {
    this->InternalColorTexture->PostRender(ren);
  }

  // timer calls take time, for lots of "small" actors
  // the timer can be a big hit. So we assume zero time
  // for anything less than 100K cells
  if (this->TimerQueryCounter == 0)
  {
    this->TimerQuery->ReusableStop();
    this->TimeToDraw = this->TimerQuery->GetReusableElapsedSeconds();
    // If the timer is not accurate enough, set it to a small
    // time so that it is not zero
    if (this->TimeToDraw == 0.0)
    {
      this->TimeToDraw = 0.0001;
    }
  }

  this->UpdateProgress(1.0);
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::ReleaseGraphicsResources(vtkWindow* win)
{
  if (!this->ResourceCallback->IsReleasing())
  {
    this->ResourceCallback->Release();
    return;
  }
  for (int i = vtkOpenGLPolyDataMapper::PrimitiveStart; i < vtkOpenGLPolyDataMapper::PrimitiveEnd;
       i++)
  {
    this->PrimitiveVBOGroup[i]->ReleaseGraphicsResources(win);
  }
  this->Superclass::ReleaseGraphicsResources(win);
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::GetShaderTemplate(
  std::map<vtkShader::Type, vtkShader*> shaders, vtkRenderer* ren, vtkActor* act)
{
  this->Superclass::GetShaderTemplate(shaders, ren, act);
  shaders[vtkShader::Geometry]->SetSource("");
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::ReplaceShaderValues(
  std::map<vtkShader::Type, vtkShader*> shaders, vtkRenderer* ren, vtkActor* act)
{
  this->ReplaceShaderPointSize(shaders, ren, act);
  this->Superclass::ReplaceShaderValues(shaders, ren, act);
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::ReplaceShaderColor(
  std::map<vtkShader::Type, vtkShader*> shaders, vtkRenderer* ren, vtkActor* act)
{
  // false so that superclass uses point color vertex attribute.
  ScopedValueRollback<bool> cellScalarsBkp(this->HaveCellScalars, false);
  this->Superclass::ReplaceShaderColor(shaders, ren, act);
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::ReplaceShaderNormal(
  std::map<vtkShader::Type, vtkShader*> shaders, vtkRenderer* ren, vtkActor* act)
{
  ScopedValueRollback<bool> cellNormalsBkp(this->HaveCellNormals, false);
  this->Superclass::ReplaceShaderNormal(shaders, ren, act);
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::ReplaceShaderCoincidentOffset(
  std::map<vtkShader::Type, vtkShader*> shaders, vtkRenderer* ren, vtkActor* act)
{
  this->Superclass::ReplaceShaderCoincidentOffset(shaders, ren, act);

  std::string FSSource = shaders[vtkShader::Fragment]->GetSource();
  // gles wants explicit type specification when mixed type arguments are used with an operand
  vtkShaderProgram::Substitute(FSSource, "cOffset/65000", "cOffset/65000.0f");
  shaders[vtkShader::Fragment]->SetSource(FSSource);
}
//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::ReplaceShaderEdges(
  std::map<vtkShader::Type, vtkShader*> shaders, vtkRenderer* ren, vtkActor* act)
{
  std::string VSSource = shaders[vtkShader::Vertex]->GetSource();
  std::string FSSource = shaders[vtkShader::Fragment]->GetSource();

  if (this->DrawingEdges(ren, act))
  {
    vtkShaderProgram::Substitute(VSSource, "//VTK::EdgesGLES30::Dec",
      "uniform vec4 vpDims;\n"
      "uniform float lineWidth;\n"
      "in float edgeValue;\n"
      "in vec4 nextVertexMC;\n"
      "in vec4 prevVertexMC;\n"
      "out vec4 edgeEqn[3];");
    vtkShaderProgram::Substitute(VSSource, "//VTK::EdgesGLES30::Impl",
      "  vec4 nextPosition = MCDCMatrix * nextVertexMC;\n"
      "  vec4 prevPosition = MCDCMatrix * prevVertexMC;\n"
      "  vec2 pos[4];\n"
      "  float vertexId = float(gl_VertexID);\n"
      "  int useID = 0;\n"
      "  if (mod(vertexId, 3.0) == 0.0)"
      "  {\n"
      "    pos[0] = gl_Position.xy/gl_Position.w;\n"
      "    pos[1] = nextPosition.xy/nextPosition.w;\n"
      "    pos[2] = prevPosition.xy/prevPosition.w;\n"
      "  }\n"
      "  else if (mod(vertexId, 3.0) == 1.0)"
      "  {\n"
      "    pos[0] = prevPosition.xy/prevPosition.w;\n"
      "    pos[1] = gl_Position.xy/gl_Position.w;\n"
      "    pos[2] = nextPosition.xy/nextPosition.w;\n"
      "    useID = 1;\n"
      "  }\n"
      "  else if (mod(vertexId, 3.0) == 2.0)"
      "  {\n"
      "    pos[0] = nextPosition.xy/nextPosition.w;\n"
      "    pos[1] = prevPosition.xy/prevPosition.w;\n"
      "    pos[2] = gl_Position.xy/gl_Position.w;\n"
      "    useID = 2;\n"
      "  }\n"

      "for(int i = 0; i < 3; ++i)\n"
      "{\n"
      "  pos[i] = pos[i]*vec2(0.5) + vec2(0.5);\n"
      "  pos[i] = pos[i]*vpDims.zw + vpDims.xy;\n"
      "}\n"
      "pos[3] = pos[0];\n"
      "float ccw = sign(cross(vec3(pos[1] - pos[0], 0.0), vec3(pos[2] - pos[0], 0.0)).z);\n"

      "for (int i = 0; i < 3; i++)\n"
      "{\n"
      "  vec2 tmp = normalize(pos[i+1] - pos[i]);\n"
      "  tmp = ccw*vec2(-tmp.y, tmp.x);\n"
      "  float d = dot(pos[i], tmp);\n"
      "  edgeEqn[i] = vec4(tmp.x, tmp.y, 0.0, -d);\n"
      "}\n"

      "vec2 offsets[3];\n"
      "offsets[0] = edgeEqn[2].xy + edgeEqn[0].xy;\n"
      "offsets[0] = -0.5*normalize(offsets[0])*lineWidth;\n"
      "offsets[0] /= vpDims.zw;\n"
      "offsets[1] = edgeEqn[0].xy + edgeEqn[1].xy;\n"
      "offsets[1] = -0.5*normalize(offsets[1])*lineWidth;\n"
      "offsets[1] /= vpDims.zw;\n"
      "offsets[2] = edgeEqn[1].xy + edgeEqn[2].xy;\n"
      "offsets[2] = -0.5*normalize(offsets[2])*lineWidth;\n"
      "offsets[2] /= vpDims.zw;\n"

      "if (edgeValue < 4.0) edgeEqn[2].z = lineWidth;\n"
      "if (mod(edgeValue, 4.0) < 2.0) edgeEqn[1].z = lineWidth;\n"
      "if (mod(edgeValue, 2.0) < 1.0) edgeEqn[0].z = lineWidth;\n"

      "gl_Position.xy = gl_Position.xy + offsets[useID]*gl_Position.w;\n");
    shaders[vtkShader::Vertex]->SetSource(VSSource);

    vtkShaderProgram::Substitute(FSSource, "//VTK::Edges::Dec",
      "in vec4 edgeEqn[3];\n"
      "uniform float lineWidth;\n"
      "uniform vec3 edgeColor;\n");

    std::string fsimpl =
      // distance gets larger as you go inside the polygon
      "float edist[3];\n"
      "edist[0] = dot(edgeEqn[0].xy, gl_FragCoord.xy) + edgeEqn[0].w;\n"
      "edist[1] = dot(edgeEqn[1].xy, gl_FragCoord.xy) + edgeEqn[1].w;\n"
      "edist[2] = dot(edgeEqn[2].xy, gl_FragCoord.xy) + edgeEqn[2].w;\n"

      // this yields wireframe only
      // "if (abs(edist[0]) > 0.5*lineWidth && abs(edist[1]) > 0.5*lineWidth && abs(edist[2]) > "
      // "0.5*lineWidth) discard;\n"

      "if (edist[0] < -0.5 && edgeEqn[0].z > 0.0) discard;\n"
      "if (edist[1] < -0.5 && edgeEqn[1].z > 0.0) discard;\n"
      "if (edist[2] < -0.5 && edgeEqn[2].z > 0.0) discard;\n"

      "edist[0] += edgeEqn[0].z;\n"
      "edist[1] += edgeEqn[1].z;\n"
      "edist[2] += edgeEqn[2].z;\n"

      "float emix = clamp(0.5 + 0.5*lineWidth - min( min( edist[0], edist[1]), edist[2]), 0.0, "
      "1.0);\n";

    bool canRenderLinesAsTube =
      act->GetProperty()->GetRenderLinesAsTubes() && ren->GetLights()->GetNumberOfItems() > 0;
    if (canRenderLinesAsTube)
    {
      fsimpl += "  diffuseColor = mix(diffuseColor, diffuseIntensity*edgeColor, emix);\n"
                "  ambientColor = mix(ambientColor, ambientIntensity*edgeColor, emix);\n"
        // " else { discard; }\n" // this yields wireframe only
        ;
    }
    else
    {
      fsimpl += "  diffuseColor = mix(diffuseColor, vec3(0.0), emix);\n"
                "  ambientColor = mix( ambientColor, edgeColor, emix);\n"
        // " else { discard; }\n" // this yields wireframe only
        ;
    }
    vtkShaderProgram::Substitute(FSSource, "//VTK::Edges::Impl", fsimpl);

    // even more fake tubes, for surface with edges this implementation
    // just adjusts the normal calculation but not the zbuffer
    if (canRenderLinesAsTube)
    {
      vtkShaderProgram::Substitute(FSSource, "//VTK::Normal::Impl",
        "//VTK::Normal::Impl\n"
        "  float cdist = min(edist[0], edist[1]);\n"
        "  vec4 cedge = mix(edgeEqn[0], edgeEqn[1], 0.5 + 0.5*sign(edist[0] - edist[1]));\n"
        "  cedge = mix(cedge, edgeEqn[2], 0.5 + 0.5*sign(cdist - edist[2]));\n"
        "  vec3 tnorm = normalize(cross(normalVCVSOutput, cross(vec3(cedge.xy,0.0), "
        "normalVCVSOutput)));\n"
        "  float rdist = 2.0*min(cdist, edist[2])/lineWidth;\n"

        // these two lines adjust for the fact that normally part of the
        // tube would be self occluded but as these are fake tubes this does
        // not happen. The code adjusts the computed location on the tube as
        // the surface normal dot view direction drops.
        "  float A = tnorm.z;\n"
        "  rdist = 0.5*rdist + 0.5*(rdist + A)/(1.0+abs(A));\n"

        "  float lenZ = clamp(sqrt(1.0 - rdist*rdist),0.0,1.0);\n"
        "  normalVCVSOutput = mix(normalVCVSOutput, normalize(rdist*tnorm + "
        "normalVCVSOutput*lenZ), emix);\n");
    }
    shaders[vtkShader::Fragment]->SetSource(FSSource);
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::ReplaceShaderPointSize(
  std::map<vtkShader::Type, vtkShader*> shaders, vtkRenderer* ren, vtkActor* act)
{
  std::string VSSource = shaders[vtkShader::Vertex]->GetSource();
  vtkShaderProgram::Substitute(VSSource, "//VTK::PointSizeGLES30::Dec", "uniform float PointSize;");
  vtkShaderProgram::Substitute(
    VSSource, "//VTK::PointSizeGLES30::Impl", "gl_PointSize = PointSize;");
  shaders[vtkShader::Vertex]->SetSource(VSSource);
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::SetMapperShaderParameters(
  vtkOpenGLHelper& cellBO, vtkRenderer* ren, vtkActor* act)
{
  this->Superclass::SetMapperShaderParameters(cellBO, ren, act);
  if (cellBO.Program->IsUniformUsed("PointSize"))
  {
    cellBO.Program->SetUniformf("PointSize", act->GetProperty()->GetPointSize());
  }
  vtkOpenGLCheckErrorMacro("failed after UpdateShader PointSize ");
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::BuildBufferObjects(vtkRenderer* ren, vtkActor* act)
{
  vtkPolyData* polydata = this->CurrentInput;
  vtkIdType vOffset = 0;

  for (auto& indexArray : this->PrimitiveIndexArrays)
  {
    indexArray.clear();
  }
  this->EdgeValues.clear();

  this->CellCellMap->SetStartOffset(0);
  this->AppendOneBufferObject(ren, act, this->CurrentInput, this->CellCellMap, vOffset);

  bool draw_surface_with_edges = (act->GetProperty()->GetEdgeVisibility() &&
    act->GetProperty()->GetRepresentation() == VTK_SURFACE);

  for (int primType = 0; primType < PrimitiveEnd; ++primType)
  {
    auto& vbos = this->PrimitiveVBOGroup[primType];
    if (draw_surface_with_edges && (primType == PrimitiveTris))
    {
      vtkNew<vtkFloatArray> edgeValuesArray;
      edgeValuesArray->SetNumberOfComponents(1);
      for (const auto& val : this->EdgeValues)
      {
        edgeValuesArray->InsertNextValue(val);
        edgeValuesArray->InsertNextValue(val);
        edgeValuesArray->InsertNextValue(val);
      }
      vbos->CacheDataArray("edgeValue", edgeValuesArray, ren, VTK_FLOAT);
    }

    for (auto name : { "vertexMC", "prevVertexMC", "nextVertexMC" })
    {
      vtkOpenGLVertexBufferObject* posVBO = vbos->GetVBO(name);
      if (posVBO)
      {

        posVBO->SetCoordShiftAndScaleMethod(
          static_cast<vtkOpenGLVertexBufferObject::ShiftScaleMethod>(this->ShiftScaleMethod));
        posVBO->SetProp3D(act);
        posVBO->SetCamera(ren->GetActiveCamera());
      }
    }

    vbos->BuildAllVBOs(ren);

    auto posVBO = vbos->GetVBO("vertexMC");
    if (posVBO)
    {
      if (posVBO->GetCoordShiftAndScaleEnabled())
      {
        std::vector<double> const& shift = posVBO->GetShift();
        std::vector<double> const& scale = posVBO->GetScale();
        this->VBOInverseTransform->Identity();
        this->VBOInverseTransform->Translate(shift[0], shift[1], shift[2]);
        this->VBOInverseTransform->Scale(1.0 / scale[0], 1.0 / scale[1], 1.0 / scale[2]);
        this->VBOInverseTransform->GetTranspose(this->VBOShiftScale);
      }
    }
  }
  this->VBOBuildTime.Modified();
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::AppendOneBufferObject(vtkRenderer* ren, vtkActor* act,
  vtkPolyData* polydata, vtkOpenGLCellToVTKCellMap* prim2cellMap, vtkIdType& voffset)
{
  vtkProperty* prop = act->GetProperty();

  if (polydata == nullptr)
  {
    return;
  }

  if (!polydata->GetPoints() || polydata->GetPoints()->GetNumberOfPoints() == 0)
  {
    return;
  }
  // Get rid of old texture color coordinates if any
  if (this->ColorCoordinates)
  {
    this->ColorCoordinates->UnRegister(this);
    this->ColorCoordinates = nullptr;
  }
  // Get rid of old texture color coordinates if any
  if (this->Colors)
  {
    this->Colors->UnRegister(this);
    this->Colors = nullptr;
  }

  this->MapScalars(polydata, 1.0);

  // If we are coloring by texture, then load the texture map.
  if (this->ColorTextureMap)
  {
    if (this->InternalColorTexture == nullptr)
    {
      this->InternalColorTexture = vtkOpenGLTexture::New();
      this->InternalColorTexture->RepeatOff();
    }
    this->InternalColorTexture->SetInputData(this->ColorTextureMap);
  }

  this->HaveCellScalars = false;
  vtkDataArray* c = this->Colors;
  if (this->ScalarVisibility)
  {
    // We must figure out how the scalars should be mapped to the polydata.
    if ((this->ScalarMode == VTK_SCALAR_MODE_USE_CELL_DATA ||
          this->ScalarMode == VTK_SCALAR_MODE_USE_CELL_FIELD_DATA ||
          this->ScalarMode == VTK_SCALAR_MODE_USE_FIELD_DATA ||
          !polydata->GetPointData()->GetScalars()) &&
      this->ScalarMode != VTK_SCALAR_MODE_USE_POINT_FIELD_DATA && this->Colors &&
      this->Colors->GetNumberOfTuples() > 0)
    {
      this->HaveCellScalars = true;
      c = nullptr;
    }
  }

  this->HaveCellNormals = false;
  // Do we have cell normals?
  vtkDataArray* n = (act->GetProperty()->GetInterpolation() != VTK_FLAT)
    ? polydata->GetPointData()->GetNormals()
    : nullptr;
  if (n == nullptr && polydata->GetCellData()->GetNormals())
  {
    this->HaveCellNormals = true;
  }

  int representation = act->GetProperty()->GetRepresentation();
  vtkCellArray* prims[4];
  prims[0] = polydata->GetVerts();
  prims[1] = polydata->GetLines();
  prims[2] = polydata->GetPolys();
  prims[3] = polydata->GetStrips();

  if (this->HaveCellScalars || this->HaveCellNormals)
  {
    prim2cellMap->Update(prims, representation, polydata->GetPoints());
  }
  prim2cellMap->BuildPrimitiveOffsetsIfNeeded(prims, representation, polydata->GetPoints());

  // Set the texture if we are going to use texture
  // for coloring with a point attribute.
  vtkDataArray* tcoords = nullptr;
  if (this->HaveTCoords(polydata))
  {
    if (this->InterpolateScalarsBeforeMapping && this->ColorCoordinates)
    {
      tcoords = this->ColorCoordinates;
    }
    else
    {
      tcoords = polydata->GetPointData()->GetTCoords();
    }
  }

  VertexAttributeArrays originalVAttribs;
  originalVAttribs.colors = c;
  originalVAttribs.normals = n;
  originalVAttribs.points = polydata->GetPoints()->GetData();
  originalVAttribs.tangents = polydata->GetPointData()->GetTangents();
  originalVAttribs.tcoords = tcoords;

  bool draw_surface_with_edges =
    (act->GetProperty()->GetEdgeVisibility() && representation == VTK_SURFACE);

  std::size_t iFirsts[PrimitiveEnd] = {};
  std::size_t iLasts[PrimitiveEnd] = {};
  for (int i = 0; i < PrimitiveEnd; ++i)
  {
    iFirsts[i] = this->PrimitiveIndexArrays[i].size();
  }
  vtkDataArray* ef = polydata->GetPointData()->GetAttribute(vtkDataSetAttributes::EDGEFLAG);
  if (ef)
  {
    if (ef->GetNumberOfComponents() != 1)
    {
      vtkDebugMacro(<< "Currently only 1d edge flags are supported.");
      ef = nullptr;
    }
    else if (!ef->IsA("vtkUnsignedCharArray"))
    {
      vtkDebugMacro(<< "Currently only unsigned char edge flags are supported.");
      ef = nullptr;
    }
  }
  vtkOpenGLES30PolyDataMapper::BuildIndexArrays(this->PrimitiveIndexArrays, this->EdgeValues, prims,
    polydata->GetPoints(), representation, draw_surface_with_edges, prop->GetVertexVisibility(),
    ef);
  for (int i = 0; i < PrimitiveEnd; ++i)
  {
    iLasts[i] = this->PrimitiveIndexArrays[i].size();
  }

  auto expand = [](vtkSmartPointer<vtkDataArray> src, vtkSmartPointer<vtkDataArray> dst,
                  const unsigned int* indices, const std::size_t numIndices) {
    if (src == nullptr || dst == nullptr)
    {
      return;
    }
    vtkExpandVertexAttributes worker;
    using DispatchT = vtkArrayDispatch::Dispatch2BySameValueType<vtkArrayDispatch::AllTypes>;
    if (!DispatchT::Execute(src.Get(), dst.Get(), worker, indices, numIndices))
    {
      worker(src.Get(), dst.Get(), indices, numIndices);
    }
  };

  const std::size_t PrimitiveSizes[PrimitiveEnd] = {
    1, // points
    2, // lines
    3, // tris,
    3, // tristrips,
    1, // verts
  };
  std::size_t primitiveStart = 0;
  for (int primType = 0; primType < PrimitiveEnd; ++primType)
  {
    VertexAttributeArrays newVertexAttrs;
    newVertexAttrs = originalVAttribs;
    auto& vbos = this->PrimitiveVBOGroup[primType];
    const auto& indexArray = this->PrimitiveIndexArrays[primType];
    const std::size_t numIndices = iLasts[primType] - iFirsts[primType];
    if (!numIndices)
    {
      continue;
    }
    const auto numPrimitives = numIndices / PrimitiveSizes[primType];
    newVertexAttrs.Resize(numIndices);
    const auto start = indexArray.data() + iFirsts[primType];
    expand(originalVAttribs.colors, newVertexAttrs.colors, start, numIndices);
    expand(originalVAttribs.normals, newVertexAttrs.normals, start, numIndices);
    expand(originalVAttribs.points, newVertexAttrs.points, start, numIndices);
    expand(originalVAttribs.tangents, newVertexAttrs.tangents, start, numIndices);
    expand(originalVAttribs.tcoords, newVertexAttrs.tcoords, start, numIndices);

    if (newVertexAttrs.points != nullptr)
    {
      vbos->AppendDataArray("vertexMC", newVertexAttrs.points, VTK_FLOAT);
    }
    if (newVertexAttrs.colors != nullptr)
    {
      vbos->AppendDataArray("scalarColor", newVertexAttrs.colors, VTK_UNSIGNED_CHAR);
    }
    else if (this->HaveCellScalars && (primType != PrimitiveVertices))
    {
      const int numComp = this->Colors->GetNumberOfComponents();
      assert(numComp == 4);
      vtkNew<vtkUnsignedCharArray> cellColors;
      cellColors->SetNumberOfComponents(4);
      const bool useFieldData =
        this->FieldDataTupleId > -1 && this->ScalarMode == VTK_SCALAR_MODE_USE_FIELD_DATA;

      // either return field tuple ID or map the primitive ID to a vtk cell ID.
      auto getDestinationColorID =
        useFieldData ? [](const std::size_t&, vtkOpenGLCellToVTKCellMap*,
                         const vtkIdType& fieldTupleId) -> vtkIdType { return fieldTupleId; }
      : [](const std::size_t& i, vtkOpenGLCellToVTKCellMap* cellmap,
          const vtkIdType&) -> vtkIdType { return cellmap->GetValue(i); };
      // for each primitive
      for (std::size_t i = 0; i < numPrimitives; ++i)
      {
        // repeat for every corner of the primitive.
        const vtkIdType destID =
          getDestinationColorID(i + primitiveStart, prim2cellMap, this->FieldDataTupleId) * numComp;
        for (std::size_t j = 0; j < PrimitiveSizes[primType]; ++j)
        {
          cellColors->InsertNextTypedTuple(this->Colors->GetPointer(destID));
        }
      }
      vbos->AppendDataArray("scalarColor", cellColors, VTK_UNSIGNED_CHAR);
    }
    if (newVertexAttrs.normals != nullptr)
    {
      vbos->AppendDataArray("normalMC", newVertexAttrs.normals, VTK_FLOAT);
    }
    else if (this->HaveCellNormals && (primType != PrimitiveVertices))
    {
      vtkDataArray* srcCellNormals = polydata->GetCellData()->GetNormals();
      const int numComp = srcCellNormals->GetNumberOfComponents();
      assert(numComp == 3);
      vtkNew<vtkFloatArray> cellNormals;
      cellNormals->SetNumberOfComponents(numComp);
      for (std::size_t i = 0; i < numPrimitives; ++i)
      {
        double* norms = srcCellNormals->GetTuple(prim2cellMap->GetValue(i + primitiveStart));
        // repeat for every corner of the primitive.
        for (std::size_t j = 0; j < PrimitiveSizes[primType]; ++j)
        {
          for (int comp = 0; comp < numComp; ++comp)
          {
            cellNormals->InsertNextValue(static_cast<float>(norms[comp]));
          }
        }
      }
      vbos->AppendDataArray("normalMC", cellNormals, VTK_FLOAT);
    }
    if (newVertexAttrs.tangents != nullptr)
    {
      vbos->AppendDataArray("tangentMC", newVertexAttrs.tangents, VTK_FLOAT);
    }
    if (newVertexAttrs.tcoords != nullptr)
    {
      vbos->AppendDataArray("tcoord", newVertexAttrs.tcoords, VTK_FLOAT);
    }
    if (draw_surface_with_edges && primType == PrimitiveTris)
    {
      vtkPopulateNeighborVertices worker;

      auto prevPoints = vtk::TakeSmartPointer(newVertexAttrs.points->NewInstance());
      prevPoints->SetNumberOfComponents(newVertexAttrs.points->GetNumberOfComponents());
      prevPoints->SetNumberOfValues(newVertexAttrs.points->GetNumberOfValues());

      auto nextPoints = vtk::TakeSmartPointer(newVertexAttrs.points->NewInstance());
      nextPoints->SetNumberOfComponents(newVertexAttrs.points->GetNumberOfComponents());
      nextPoints->SetNumberOfValues(newVertexAttrs.points->GetNumberOfValues());

      using Dispatch3T = vtkArrayDispatch::Dispatch3BySameValueType<vtkArrayDispatch::AllTypes>;
      constexpr int primitiveSize = 3;
      if (!Dispatch3T::Execute(
            newVertexAttrs.points.Get(), prevPoints.Get(), nextPoints.Get(), worker, primitiveSize))
      {
        worker(newVertexAttrs.points.Get(), prevPoints.Get(), nextPoints.Get(), primitiveSize);
      }

      vbos->AppendDataArray("prevVertexMC", prevPoints, VTK_FLOAT);
      vbos->AppendDataArray("nextVertexMC", nextPoints, VTK_FLOAT);
    }
    primitiveStart += numPrimitives;
  }
}

//------------------------------------------------------------------------------
void vtkOpenGLES30PolyDataMapper::BuildIndexArrays(
  std::vector<unsigned int> (&indexArrays)[PrimitiveEnd], std::vector<unsigned char>& edgeArray,
  vtkCellArray* prims[4], vtkPoints* points, int representation,
  bool draw_surf_with_edges /*=false*/, bool vertex_visibility /*=false*/,
  vtkDataArray* ef /*= nullptr*/)
{
  typedef vtkOpenGLIndexBufferObject oglIndexUtils; // shorter
  oglIndexUtils::AppendPointIndexBuffer(indexArrays[PrimitivePoints], prims[0], 0);
  if (representation == VTK_POINTS)
  {
    oglIndexUtils::AppendPointIndexBuffer(indexArrays[PrimitiveLines], prims[1], 0);
    oglIndexUtils::AppendPointIndexBuffer(indexArrays[PrimitiveTris], prims[2], 0);
    oglIndexUtils::AppendPointIndexBuffer(indexArrays[PrimitiveTriStrips], prims[3], 0);
  }
  else // WIREFRAME OR SURFACE
  {
    oglIndexUtils::AppendLineIndexBuffer(indexArrays[PrimitiveLines], prims[1], 0);
    if (representation == VTK_WIREFRAME)
    {
      if (ef)
      {
        oglIndexUtils::AppendEdgeFlagIndexBuffer(indexArrays[PrimitiveTris], prims[2], 0, ef);
      }
      else
      {
        oglIndexUtils::AppendTriangleLineIndexBuffer(indexArrays[PrimitiveTris], prims[2], 0);
      }
    }
    else
    {
      if (draw_surf_with_edges)
      {
        oglIndexUtils::AppendTriangleIndexBuffer(
          indexArrays[PrimitiveTris], prims[2], points, 0, &edgeArray, ef);
      }
      else
      {
        oglIndexUtils::AppendTriangleIndexBuffer(
          indexArrays[PrimitiveTris], prims[2], points, 0, nullptr, nullptr);
      }
    }
    oglIndexUtils::AppendStripIndexBuffer(
      indexArrays[PrimitiveTriStrips], prims[3], 0, representation == VTK_WIREFRAME);
  }
  // vertex visibility implies that all vertices of all primitives need to be shown.
  if (vertex_visibility)
  {
    oglIndexUtils::AppendVertexIndexBuffer(indexArrays[PrimitiveVertices], prims, 0);
  }
}
