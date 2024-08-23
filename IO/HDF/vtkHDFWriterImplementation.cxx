// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause
#include "vtkHDFWriterImplementation.h"

#include "vtkHDF5ScopedHandle.h"
#include "vtkHDFVersion.h"
#include "vtkLogger.h"

#include "vtk_hdf5.h"

#include <algorithm>
#include <numeric>

VTK_ABI_NAMESPACE_BEGIN

namespace PATH
{
// VTKHDF Group & Dataset paths definitions, used to create virtual datasets properly in meta-files.
const std::string POINTS{ "Points" };
const std::string OFFSETS{ "Offsets" };
const std::string TYPES{ "Types" };
const std::string CONNECTIVITY{ "Connectivity" };

const std::string NUMBER_OF_POINTS{ "/VTKHDF/NumberOfPoints" };
const std::string NUMBER_OF_CELLS{ "/VTKHDF/NumberOfCells" };
const std::string CELL_DATA{ "/VTKHDF/CellData" };
const std::string POINT_DATA{ "/VTKHDF/PointData" };

const std::string STEPS_POINT_OFFSETS{ "/VTKHDF/Steps/PointOffsets" };
const std::string STEPS_CELL_OFFSETS{ "/VTKHDF/Steps/CellOffsets" };
const std::string STEPS_CONNECTIVITY_ID_OFFSETS{ "/VTKHDF/Steps/ConnectivityIdOffsets" };

const std::array<std::string, 3> SINGLE_VALUES = { "NumberOfPoints", "NumberOfCells",
  "NumberOfConnectivityIds" };
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::WriteHeader(hid_t group, const char* hdfType)
{
  // Write type attribute to root
  std::string strType{ hdfType };
  vtkHDF::ScopedH5SHandle scalarSpaceAttribute{ H5Screate(H5S_SCALAR) };
  if (scalarSpaceAttribute == H5I_INVALID_HID)
  {
    return false;
  }
  vtkHDF::ScopedH5PHandle utf8PropertyList{ H5Pcreate(H5P_ATTRIBUTE_CREATE) };
  if (utf8PropertyList == H5I_INVALID_HID)
  {
    return false;
  }
  if (H5Pset_char_encoding(utf8PropertyList, H5T_CSET_UTF8) < 0)
  {
    return false;
  }
  vtkHDF::ScopedH5THandle typeOfTypeAttr = H5Tcreate(H5T_STRING, strType.length());
  if (typeOfTypeAttr == H5I_INVALID_HID)
  {
    return false;
  }
  vtkHDF::ScopedH5AHandle typeAttribute{ H5Acreate(
    group, "Type", typeOfTypeAttr, scalarSpaceAttribute, utf8PropertyList, H5P_DEFAULT) };
  if (typeAttribute == H5I_INVALID_HID)
  {
    return false;
  }
  if (H5Awrite(typeAttribute, typeOfTypeAttr, strType.c_str()) < 0)
  {
    return false;
  }

  // Write version attribute to root
  hsize_t versionDimensions[1] = { 2 };
  int versionBuffer[2] = { vtkHDFMajorVersion, vtkHDFMinorVersion };
  vtkHDF::ScopedH5SHandle versionDataspace{ H5Screate(H5S_SIMPLE) };
  if (versionDataspace == H5I_INVALID_HID)
  {
    return false;
  }
  if (H5Sset_extent_simple(versionDataspace, 1, versionDimensions, versionDimensions) < 0)
  {
    return false;
  }
  vtkHDF::ScopedH5AHandle versionAttribute =
    H5Acreate(group, "Version", H5T_STD_I64LE, versionDataspace, H5P_DEFAULT, H5P_DEFAULT);
  if (versionAttribute == H5I_INVALID_HID)
  {
    return false;
  }
  if (H5Awrite(versionAttribute, H5T_NATIVE_INT, versionBuffer) < 0)
  {
    return false;
  }

  this->HdfType = hdfType;
  return true;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::CreateFile(bool overwrite, const std::string& filename)
{
  // Create file
  vtkDebugWithObjectMacro(
    this->Writer, << "Creating file " << this->Writer->CurrentPiece << ": " << filename);

  vtkHDF::ScopedH5FHandle file{ H5Fcreate(
    filename.c_str(), overwrite ? H5F_ACC_TRUNC : H5F_ACC_EXCL, H5P_DEFAULT, H5P_DEFAULT) };
  if (file == H5I_INVALID_HID)
  {
    return false;
  }

  // Create the root group
  vtkHDF::ScopedH5GHandle root = this->CreateHdfGroupWithLinkOrder(file, "VTKHDF");
  if (root == H5I_INVALID_HID)
  {
    return false;
  }

  this->File = std::move(file);
  this->Root = std::move(root);

  return true;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::OpenFile()
{
  const char* filename = this->Writer->GetFileName();
  vtkDebugWithObjectMacro(
    this->Writer, << "Opening file on rank" << this->Writer->CurrentPiece << ": " << filename);

  vtkHDF::ScopedH5FHandle file{ H5Fopen(filename, H5F_ACC_RDWR, H5P_DEFAULT) };
  if (file == H5I_INVALID_HID)
  {
    return false;
  }

  this->File = std::move(file);
  this->Root = this->OpenExistingGroup(this->File, "VTKHDF");

  return this->Root != H5I_INVALID_HID;
}

//------------------------------------------------------------------------------
void vtkHDFWriter::Implementation::CloseFile()
{
  vtkDebugWithObjectMacro(this->Writer,
    "Closing current file " << this->File << this->Writer->FileName << " on rank "
                            << this->Writer->CurrentPiece);

  // Setting to H5I_INVALID_HID closes the group/file using RAII
  this->StepsGroup = H5I_INVALID_HID;
  this->Root = H5I_INVALID_HID;
  this->File = H5I_INVALID_HID;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::OpenSubfile(const std::string& filename)
{
  vtkDebugWithObjectMacro(
    this->Writer, << "Opening sub file on rank " << this->Writer->CurrentPiece << ": " << filename);

  vtkHDF::ScopedH5FHandle file{ H5Fopen(filename.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT) };
  if (file == H5I_INVALID_HID)
  {
    return false;
  }

  this->Subfiles.emplace_back(std::move(file));
  this->SubfileNames.emplace_back(filename);

  return true;
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5GHandle vtkHDFWriter::Implementation::OpenExistingGroup(
  hid_t group, const char* name)
{
  vtkDebugWithObjectMacro(this->Writer, << "Opening group " << name);
  return H5Gopen(group, name, H5P_DEFAULT);
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::OpenDataset(hid_t group, const char* name)
{
  return H5Dopen(group, name, H5P_DEFAULT);
}

//------------------------------------------------------------------------------
std::string vtkHDFWriter::Implementation::GetGroupName(hid_t group)
{
  size_t len = H5Iget_name(group, nullptr, 0);
  if (len == 0)
  {
    return std::string("");
  }
  std::string buffer;
  buffer.resize(len);
  H5Iget_name(group, &buffer.front(), len + 1);
  return buffer;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::CreateStepsGroup()
{
  vtkHDF::ScopedH5GHandle stepsGroup{ H5Gcreate(
    this->Root, "Steps", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT) };
  if (stepsGroup == H5I_INVALID_HID)
  {
    return false;
  }

  this->StepsGroup = std::move(stepsGroup);
  return true;
}

//------------------------------------------------------------------------------
std::vector<vtkHDFWriter::Implementation::PolyDataTopos>
vtkHDFWriter::Implementation::GetCellArraysForTopos(vtkPolyData* polydata)
{
  return {
    { "Vertices", polydata->GetVerts() },
    { "Lines", polydata->GetLines() },
    { "Polygons", polydata->GetPolys() },
    { "Strips", polydata->GetStrips() },
  };
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::CreateAndWriteHdfDataset(hid_t group,
  hid_t type, hid_t source_type, const char* name, int rank, std::vector<hsize_t> dimensions,
  const void* data)
{
  // Create the dataspace, use the whole extent
  vtkHDF::ScopedH5SHandle dataspace = this->CreateSimpleDataspace(rank, dimensions.data());
  if (dataspace == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  // Create the dataset from the dataspace and other arguments
  vtkHDF::ScopedH5DHandle dataset = this->CreateHdfDataset(group, name, type, dataspace);
  if (dataset == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  // Write to the dataset
  if (source_type == H5T_C_S1)
  {
    source_type = H5Tcopy(H5T_C_S1);
    H5Tset_size(source_type, H5T_VARIABLE);
  }

  if (data != nullptr && H5Dwrite(dataset, source_type, H5S_ALL, dataspace, H5P_DEFAULT, data) < 0)
  {
    return H5I_INVALID_HID;
  }

  return dataset;
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5SHandle vtkHDFWriter::Implementation::CreateSimpleDataspace(
  int rank, const hsize_t dimensions[])
{
  vtkHDF::ScopedH5SHandle dataspace{ H5Screate(H5S_SIMPLE) };
  if (dataspace == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }

  auto res = H5Sset_extent_simple(dataspace, rank, dimensions, dimensions);
  if (res < 0)
  {
    return H5I_INVALID_HID;
  }
  return dataspace;
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5AHandle vtkHDFWriter::Implementation::CreateScalarAttribute(
  hid_t group, const char* name, int value)
{
  if (H5Aexists(group, name))
  {
    return H5Aopen_name(group, name);
  }

  vtkHDF::ScopedH5SHandle scalarSpaceAttribute{ H5Screate(H5S_SCALAR) };
  if (scalarSpaceAttribute == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }

  vtkHDF::ScopedH5AHandle nStepsAttribute{ H5Acreate(
    group, name, H5T_STD_I64LE, scalarSpaceAttribute, H5P_DEFAULT, H5P_DEFAULT) };
  if (nStepsAttribute == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }

  if (H5Awrite(nStepsAttribute, H5T_NATIVE_INT, &value) < 0)
  {
    return H5I_INVALID_HID;
  }

  return nStepsAttribute;
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5SHandle vtkHDFWriter::Implementation::CreateUnlimitedSimpleDataspace(
  hsize_t numCols)
{
  int numDims = (numCols == 1) ? 1 : 2;
  std::vector<hsize_t> dims{ 0 };
  std::vector<hsize_t> max_dims{ H5S_UNLIMITED };

  if (numDims == 2)
  {
    dims.emplace_back(numCols);
    // as the number of cols cannot change, ensure to block it to the initial number of column
    max_dims.emplace_back(numCols);
  }

  vtkHDF::ScopedH5SHandle dataspace{ H5Screate_simple(numDims, dims.data(), max_dims.data()) };
  if (dataspace == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  return dataspace;
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5GHandle vtkHDFWriter::Implementation::CreateHdfGroup(hid_t group, const char* name)
{
  return H5Gcreate(group, name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5GHandle vtkHDFWriter::Implementation::CreateHdfGroupWithLinkOrder(
  hid_t group, const char* name)
{
  vtkHDF::ScopedH5PHandle plist = H5Pcreate(H5P_GROUP_CREATE);
  if (plist == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  H5Pset_link_creation_order(plist, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);
  return H5Gcreate(group, name, H5P_DEFAULT, plist, H5P_DEFAULT);
}

//------------------------------------------------------------------------------
herr_t vtkHDFWriter::Implementation::CreateSoftLink(
  hid_t group, const char* groupName, const char* targetLink)
{
  return H5Lcreate_soft(targetLink, group, groupName, H5P_DEFAULT, H5P_DEFAULT);
}

//------------------------------------------------------------------------------
herr_t vtkHDFWriter::Implementation::CreateExternalLink(
  hid_t group, const char* filename, const char* source, const char* targetLink)
{
  return H5Lcreate_external(filename, source, group, targetLink, H5P_DEFAULT, H5P_DEFAULT);
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::CreateHdfDataset(
  hid_t group, const char* name, hid_t type, hid_t dataspace)
{
  return H5Dcreate(group, name, type, dataspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::CreateHdfDataset(
  hid_t group, const char* name, hid_t type, int rank, const hsize_t dimensions[])
{
  vtkHDF::ScopedH5SHandle dataspace = this->CreateSimpleDataspace(rank, dimensions);
  if (dataspace == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  return this->CreateHdfDataset(group, name, type, dataspace);
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::CreateChunkedHdfDataset(hid_t group,
  const char* name, hid_t type, hid_t dataspace, hsize_t numCols, hsize_t chunkSize[],
  int compressionLevel)
{
  vtkHDF::ScopedH5PHandle plist = H5Pcreate(H5P_DATASET_CREATE);
  if (plist == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  H5Pset_layout(plist, H5D_CHUNKED);
  if (numCols == 1)
  {
    H5Pset_chunk(plist, 1, chunkSize);
  }
  else
  {
    H5Pset_chunk(plist, 2, chunkSize); // 2-Dimensional
  }

  if (compressionLevel != 0)
  {
    H5Pset_deflate(plist, compressionLevel);
  }

  vtkHDF::ScopedH5DHandle dset =
    H5Dcreate(group, name, type, dataspace, H5P_DEFAULT, plist, H5P_DEFAULT);
  if (dset == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  return dset;
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5SHandle vtkHDFWriter::Implementation::CreateDataspaceFromArray(
  vtkAbstractArray* dataArray)
{
  const int nComp = dataArray->GetNumberOfComponents();
  const int nTuples = dataArray->GetNumberOfTuples();
  const hsize_t dimensions[] = { (hsize_t)nTuples, (hsize_t)nComp };
  const int rank = nComp > 1 ? 2 : 1;
  return CreateSimpleDataspace(rank, dimensions);
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::CreateDatasetFromDataArray(
  hid_t group, const char* name, hid_t type, vtkAbstractArray* dataArray)
{
  // Create dataspace from array
  vtkHDF::ScopedH5SHandle dataspace = CreateDataspaceFromArray(dataArray);
  if (dataspace == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  // Create dataset from dataspace and other arguments
  vtkHDF::ScopedH5DHandle dataset = this->CreateHdfDataset(group, name, type, dataspace);
  if (dataset == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  // Get the data pointer
  void* data = dataArray->GetVoidPointer(0);
  // If there is no data pointer, return either an invalid id or the dataset depending on the number
  // of values in the dataArray
  if (data == nullptr)
  {
    if (dataArray->GetNumberOfValues() == 0)
    {
      return dataset;
    }
    else
    {
      return H5I_INVALID_HID;
    }
  }
  // Find which HDF type corresponds to the dataArray type
  // It is different from the `type` in the argument list which defines which type should be used to
  // store the data in the HDF file
  hid_t source_type = vtkHDFUtilities::getH5TypeFromVtkType(dataArray->GetDataType());
  if (source_type == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }
  // Write vtkAbstractArray data to the HDF dataset
  if (H5Dwrite(dataset, source_type, H5S_ALL, dataspace, H5P_DEFAULT, data) < 0)
  {
    return H5I_INVALID_HID;
  }
  return dataset;
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::CreateSingleValueDataset(
  hid_t group, const char* name, int value)
{
  return this->Create2DValueDataset(group, name, &value, 1);
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::Create2DValueDataset(
  hid_t group, const char* name, int* value, int size)
{
  if (size != 2)
  {
    // not a warn or an error as it could be noisy depending on the size of the data
    vtkLog(INFO, "Try to create a 2D dataset with a size of " + std::to_string(size));
    return H5I_INVALID_HID;
  }
  std::vector<hsize_t> dimensions;
  dimensions.push_back(2);

  return this->CreateAndWriteHdfDataset(
    group, H5T_STD_I64LE, H5T_NATIVE_INT, name, 1, dimensions, &value);
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::AddSingleValueToDataset(
  hid_t dataset, int value, bool offset, bool trim)
{
  vtkDebugWithObjectMacro(this->Writer, "Adding 1 value to " << this->GetGroupName(dataset));
  // Create a new dataspace containing a single value
  const hsize_t addedDims[1] = { 1 };
  vtkHDF::ScopedH5SHandle newDataspace = H5Screate_simple(1, addedDims, nullptr);
  if (newDataspace == H5I_INVALID_HID)
  {
    return false;
  }

  // Recover dataset and dataspace
  vtkHDF::ScopedH5SHandle currentDataspace = H5Dget_space(dataset);
  if (currentDataspace == H5I_INVALID_HID)
  {
    return false;
  }

  // Retrieve current dataspace dimensions
  hsize_t currentdims[1] = { 0 };
  H5Sget_simple_extent_dims(currentDataspace, currentdims, nullptr);
  const hsize_t newdims[1] = { currentdims[0] + addedDims[0] };

  // Add the last value of the dataset if we want an offset (only for arrays of stride 1)
  if (offset && currentdims[0] > 0)
  {
    std::vector<int> allValues;
    allValues.resize(currentdims[0]);
    H5Dread(dataset, H5T_NATIVE_INT, currentDataspace, H5S_ALL, H5P_DEFAULT, allValues.data());
    value += allValues.back();
  }

  // Resize dataset
  if (!trim)
  {
    if (H5Dset_extent(dataset, newdims) < 0)
    {
      return false;
    }
    currentDataspace = H5Dget_space(dataset);
    if (currentDataspace == H5I_INVALID_HID)
    {
      return false;
    }
  }
  hsize_t start[1] = { currentdims[0] - trim };
  hsize_t count[1] = { addedDims[0] };
  H5Sselect_hyperslab(currentDataspace, H5S_SELECT_SET, start, nullptr, count, nullptr);

  // Write new data to the dataset
  if (H5Dwrite(dataset, H5T_NATIVE_INT, newDataspace, currentDataspace, H5P_DEFAULT, &value) < 0)
  {
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::AddFieldDataSizeValueToDataset(
  hid_t dataset, int* value, int size, bool offset)
{
  if (size <= 1)
  {
    vtkLog(
      WARNING, "Size given in this method shouldn't be less than 2, got : " + std::to_string(size));
    return false;
  }

  // Specific value linked to how the VTKHDF File Format works for temporal field data offset
  std::vector<hsize_t> addedDims{ 1, 2 };
  std::vector<hsize_t> maxDims{ H5S_UNLIMITED, 2 };

  vtkHDF::ScopedH5SHandle newDataspace = H5Screate_simple(1, addedDims.data(), maxDims.data());
  if (newDataspace == H5I_INVALID_HID)
  {
    return false;
  }

  // Recover dataset and dataspace
  vtkHDF::ScopedH5SHandle currentDataspace = H5Dget_space(dataset);
  if (currentDataspace == H5I_INVALID_HID)
  {
    return false;
  }

  // Retrieve current dataspace dimensions
  int nbDims = H5Sget_simple_extent_ndims(currentDataspace);
  if (nbDims <= 0)
  {
    return true;
  }

  std::vector<hsize_t> currentdims(nbDims);
  H5Sget_simple_extent_dims(currentDataspace, currentdims.data(), nullptr);

  std::vector<hsize_t> newdims(nbDims);
  newdims[0] = { currentdims[0] + addedDims[0] };
  for (int i = 1; i < nbDims; i++)
  {
    newdims[i] = currentdims[i];
  }

  // Add the last value of the dataset if we want an offset (only for arrays of stride 1)
  if (offset && currentdims[0] > 0)
  {
    std::vector<int> allValues(size);
    H5Dread(dataset, H5T_NATIVE_INT, currentDataspace, H5S_ALL, H5P_DEFAULT, allValues.data());
    if (size == 1)
    {
      value[0] += allValues.back();
    }
  }

  // Resize dataset
  if (H5Dset_extent(dataset, newdims.data()) < 0)
  {
    return false;
  }
  currentDataspace = H5Dget_space(dataset);
  if (currentDataspace == H5I_INVALID_HID)
  {
    return false;
  }

  // create the hyperslab
  hsize_t start[2] = { currentdims[0], 0 };
  hsize_t count[2] = { 1, 1 };
  hsize_t block[2] = { 1, 2 };
  H5Sselect_hyperslab(currentDataspace, H5S_SELECT_SET, start, nullptr, count, block);
  hsize_t length[1] = { 2 };
  newDataspace = H5Screate_simple(1, length, nullptr);

  // Write new data to the dataset
  if (H5Dwrite(dataset, H5T_NATIVE_INT, newDataspace, currentDataspace, H5P_DEFAULT, value) < 0)
  {
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::AddOrCreateSingleValueDataset(
  hid_t group, const char* name, int value, bool offset, bool trim)
{
  // Assume that when subfiles are set, we don't need to write data unless
  // SubFilesReady is set, which means all subfiles have been written.
  if (!this->Subfiles.empty() && (group != this->StepsGroup || this->Writer->NbPieces > 1))
  {
    if (this->SubFilesReady)
    {
      return this->CreateVirtualDataset(group, name, H5T_STD_I64LE, 1) != H5I_INVALID_HID;
    }
    return true;
  }

  if (!H5Lexists(group, name, H5P_DEFAULT))
  {
    // Dataset needs to be created
    return this->CreateSingleValueDataset(group, name, value) != H5I_INVALID_HID;
  }
  else
  {
    // Append the value to an existing dataset
    vtkHDF::ScopedH5DHandle dataset = H5Dopen(group, name, H5P_DEFAULT);
    if (dataset == H5I_INVALID_HID)
    {
      return false;
    }
    return this->AddSingleValueToDataset(dataset, value, offset, trim);
  }
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::AddOrCreateFieldDataSizeValueDataset(
  hid_t group, const char* name, int* value, int size, bool offset)
{
  // Assume that when subfiles are set, we don't need to write data unless
  // SubFilesReady is set, which means all subfiles have been written.
  if (!this->Subfiles.empty() && group != this->StepsGroup)
  {
    if (this->SubFilesReady)
    {
      return this->CreateVirtualDataset(group, name, H5T_STD_I64LE, 1) != H5I_INVALID_HID;
    }
    return true;
  }
  if (!H5Lexists(group, name, H5P_DEFAULT))
  {
    // Dataset needs to be created
    return this->Create2DValueDataset(group, name, value, size) != H5I_INVALID_HID;
  }
  else
  {
    // Append the value to an existing dataset
    vtkHDF::ScopedH5DHandle dataset = H5Dopen(group, name, H5P_DEFAULT);
    return this->AddFieldDataSizeValueToDataset(dataset, value, size, offset);
  }
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::AddArrayToDataset(
  hid_t dataset, vtkAbstractArray* dataArray, int trim)
{
  if (dataArray == nullptr)
  {
    return true;
  }

  if (dataset == -1)
  {
    return false;
  }

  // Get raw array data
  void* rawArrayData = dataArray->GetVoidPointer(0);
  if (rawArrayData == nullptr)
  {
    if (dataArray->GetNumberOfValues() == 0)
    {
      return true;
    }
    else
    {
      return false;
    }
  }

  hid_t source_type = vtkHDFUtilities::getH5TypeFromVtkType(dataArray->GetDataType());
  if (source_type == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }

  // Create dataspace from array
  vtkHDF::ScopedH5SHandle dataspace = this->CreateDataspaceFromArray(dataArray);
  if (dataspace == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }

  // Recover dataspace
  vtkHDF::ScopedH5SHandle currentDataspace = H5Dget_space(dataset);
  if (currentDataspace == H5I_INVALID_HID)
  {
    return false;
  }

  // Retrieve current dataspace dimensions
  const int nComp = dataArray->GetNumberOfComponents();
  const int numDim = nComp == 1 ? 1 : 2;
  int nTuples = dataArray->GetNumberOfTuples();

  std::vector<hsize_t> addedDims{ (hsize_t)nTuples };
  std::vector<hsize_t> currentdims(numDim, 0);
  if (numDim == 2)
  {
    addedDims.emplace_back(nComp);
  }

  H5Sget_simple_extent_dims(currentDataspace, currentdims.data(), nullptr);
  std::vector<hsize_t> newdims = { currentdims[0] + addedDims[0] };
  if (numDim == 2)
  {
    newdims.emplace_back(currentdims[1]);

    if (currentdims[1] != addedDims[1])
    {
      return false; // Number of components don't match
    }
  }

  if (addedDims[0] - trim > 0)
  {
    // Resize existing dataset to make space for the added array
    H5Dset_extent(dataset, newdims.data());
  }
  currentDataspace = H5Dget_space(dataset);
  if (currentDataspace == H5I_INVALID_HID)
  {
    return false;
  }
  std::vector<hsize_t> start{ currentdims[0] - trim };
  std::vector<hsize_t> count{ addedDims[0] };
  if (numDim == 2)
  {
    start.emplace_back(0);
    count.emplace_back(addedDims[1]);
  }

  if (dataArray->GetDataType() == VTK_STRING)
  {
    auto vtkStrArray = vtkStringArray::SafeDownCast(dataArray);
    if (vtkStrArray == nullptr)
    {
      return false;
    }

    hid_t datatype = H5Tcopy(H5T_C_S1);
    H5Tset_size(datatype, H5T_VARIABLE);

    std::vector<const char*> strArray;
    strArray.resize(vtkStrArray->GetNumberOfValues());
    for (vtkIdType i = 0; i < vtkStrArray->GetNumberOfValues(); i++)
    {
      strArray[i] = vtkStrArray->GetPointer(i)->c_str();
    }

    if (strArray.empty())
    {
      return true;
    }

    H5Sselect_hyperslab(
      currentDataspace, H5S_SELECT_SET, start.data(), nullptr, count.data(), nullptr);

    // Write new data to the dataset
    if (H5Dwrite(dataset, datatype, dataspace, currentDataspace, H5P_DEFAULT, strArray.data()) < 0)
    {
      return false;
    }
  }
  else
  {
    H5Sselect_hyperslab(
      currentDataspace, H5S_SELECT_SET, start.data(), nullptr, count.data(), nullptr);

    // Write new data to the dataset
    if (H5Dwrite(dataset, source_type, dataspace, currentDataspace, H5P_DEFAULT, rawArrayData) < 0)
    {
      return false;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::AddOrCreateDataset(
  hid_t group, const char* name, hid_t type, vtkAbstractArray* dataArray)
{
  if (!this->Subfiles.empty())
  {
    if (this->SubFilesReady)
    {
      return this->CreateVirtualDataset(group, name, type, dataArray->GetNumberOfComponents()) !=
        H5I_INVALID_HID;
    }
    return true;
  }

  if (!H5Lexists(group, name, H5P_DEFAULT))
  {
    // Dataset needs to be created
    return this->CreateDatasetFromDataArray(group, name, type, dataArray) != H5I_INVALID_HID;
  }
  else
  {
    // Simply append the array to an existing dataset
    vtkHDF::ScopedH5DHandle dataset = H5Dopen(group, name, H5P_DEFAULT);
    if (dataset == -1)
    {
      return false;
    }
    return this->AddArrayToDataset(dataset, dataArray);
  }
}

//------------------------------------------------------------------------------
vtkHDF::ScopedH5DHandle vtkHDFWriter::Implementation::CreateVirtualDataset(
  hid_t group, const char* name, hid_t type, int numComp)
{
  vtkDebugWithObjectMacro(
    this->Writer, "Creating virtual dataset " << name << " in " << this->GetGroupName(group));

  if (group == this->StepsGroup)
  {
    return this->WriteSumSteps(group, name);
  }

  // Initialize Virtual Dataset property
  vtkHDF::ScopedH5PHandle virtualSourceP = H5Pcreate(H5P_DATASET_CREATE);
  if (virtualSourceP == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }

  // Collect total dataset size
  const std::string datasetPath = this->GetGroupName(group) + "/" + name;
  hsize_t totalSize = 0;
  if (!this->GetSubFilesDatasetSize(
        datasetPath.c_str(), this->GetGroupName(group).c_str(), totalSize))
  {
    vtkDebugWithObjectMacro(
      this->Writer, << "Ignoring dataset " << datasetPath << " not present in every sub-file.");
    if (this->GetGroupName(group) == PATH::CELL_DATA ||
      this->GetGroupName(group) == PATH::POINT_DATA)
    {
      return group; // Partial field, no error
    }
    return H5I_INVALID_HID;
  }
  vtkDebugWithObjectMacro(
    this->Writer, "Total Virtual Dataset Size: " << totalSize << "x" << numComp);

  // Create destination dataspace with the final size
  std::vector<hsize_t> dspaceDims{ totalSize };
  const int numDim = numComp == 1 ? 1 : 2;
  if (numDim == 2)
  {
    dspaceDims.emplace_back(numComp);
  }
  vtkHDF::ScopedH5SHandle destSpace = H5Screate_simple(numDim, dspaceDims.data(), nullptr);
  if (virtualSourceP == H5I_INVALID_HID)
  {
    return H5I_INVALID_HID;
  }

  // Find if dataset is indexed on points, cells, or connectivity, or is a meta-data array
  IndexingMode indexMode = this->GetDatasetIndexationMode(group, name);

  // Find primitive for PolyData
  bool isPolyData = this->HdfType == "PolyData";
  char primitive = isPolyData ? this->GetPrimitive(group) : 0;

  hsize_t totalSteps = 1;
  if (this->Writer->IsTemporal && this->Writer->NbPieces != 1)
  {
    totalSteps = this->Writer->NumberOfTimeSteps;
  }

  // Keep track of offsets in the destination, and in each of the source datasets.
  hsize_t mappingOffset = 0;
  std::vector<hsize_t> sourceOffsets(this->Subfiles.size(), 0);

  // Store previous source offsets to handle static meshes
  std::vector<hsize_t> prevOffsets(this->Subfiles.size(), 0);

  // Build virtual dataset mappings from sub-files, based on time steps and parts
  for (hsize_t step = 0; step < totalSteps; step++)
  {
    for (hsize_t part = 0; part < this->Subfiles.size(); part++)
    {
      // Open source dataset/dataspace
      vtkHDF::ScopedH5DHandle sourceDataset =
        H5Dopen(this->Subfiles[part], datasetPath.c_str(), H5P_DEFAULT);
      if (sourceDataset == H5I_INVALID_HID)
      {
        return H5I_INVALID_HID;
      }
      vtkHDF::ScopedH5SHandle sourceDataSpace = H5Dget_space(sourceDataset);
      if (sourceDataSpace == H5I_INVALID_HID)
      {
        return H5I_INVALID_HID;
      }
      std::array<hsize_t, 3> sourceDims{ 0, 0, 0 };
      if (H5Sget_simple_extent_dims(sourceDataSpace, sourceDims.data(), nullptr) < 0)
      {
        return H5I_INVALID_HID;
      }

      std::vector<hsize_t> mappingSize{
        sourceDims[0]
      }; // By default, select the whole source dataset

      switch (indexMode)
      {
        case IndexingMode::MetaData:
        {
          // Select only one value in the source dataspace
          mappingSize[0] = 1;
          break;
        }
        case IndexingMode::Points:
        {
          // Handle static mesh
          if (name == PATH::POINTS && totalSteps > 1)
          {
            hsize_t partPointsOffset =
              this->GetSubfileNumberOf(PATH::STEPS_POINT_OFFSETS, part, step);
            if (step > 0 && prevOffsets[part] == partPointsOffset)
            {
              vtkDebugWithObjectMacro(
                this->Writer, << "Static mesh, not writing points virtual dataset again");
              continue;
            }
            prevOffsets[part] = partPointsOffset;
          }

          mappingSize[0] = this->GetSubfileNumberOf(PATH::NUMBER_OF_POINTS, part, step);
          break;
        }
        case IndexingMode::Cells:
        {
          vtkDebugWithObjectMacro(this->Writer, << "Is Indexed on cells");
          hsize_t partNbCells =
            this->GetNumberOfCellsSubfile(part, step, isPolyData, this->GetGroupName(group));

          // Handle static mesh: don't write offsets if cells have not changed
          if ((name == PATH::OFFSETS || name == PATH::TYPES) && totalSteps > 1)
          {
            hsize_t partCellOffset =
              this->GetSubfileNumberOf(PATH::STEPS_CELL_OFFSETS, part, step, primitive);
            if (step > 0 && prevOffsets[part] == partCellOffset)
            {
              vtkDebugWithObjectMacro(
                this->Writer, << "Static mesh, not writing virtual offsets again");
              continue;
            }
            prevOffsets[part] = partCellOffset;
          }

          mappingSize[0] = partNbCells;

          // For N cells, store N+1 cell offsets
          if (name == PATH::OFFSETS)
          {
            mappingSize[0]++;
          }
          break;
        }
        case IndexingMode::Connectivity:
        {
          // Handle static mesh
          if (name == PATH::CONNECTIVITY && totalSteps > 1)
          {
            hsize_t partConnOffset =
              this->GetSubfileNumberOf(PATH::STEPS_CONNECTIVITY_ID_OFFSETS, part, step, primitive);
            if (step > 0 && prevOffsets[part] == partConnOffset)
            {
              vtkDebugWithObjectMacro(
                this->Writer, << "Static mesh, not writing virtual connectivity Ids again");
              continue;
            }
            prevOffsets[part] = partConnOffset;
          }

          mappingSize[0] = this->GetSubfileNumberOf(
            this->GetGroupName(group) + "/NumberOfConnectivityIds", part, step);
          break;
        }
        default:
          break;
      }

      // Select hyperslab in source space of size 1
      std::vector<hsize_t> sourceOffset{ sourceOffsets[part] };

      if (numDim == 2)
      {
        sourceOffset.emplace_back(0);
        mappingSize.emplace_back(sourceDims[1]); // All components
      }

      // Select hyperslab in destination space
      std::vector<hsize_t> destinationOffset{ mappingOffset };
      if (numDim == 2)
      {
        destinationOffset.emplace_back(0);
      }
      if (H5Sselect_hyperslab(destSpace, H5S_SELECT_SET, destinationOffset.data(), nullptr,
            mappingSize.data(), nullptr) < 0)
      {
        return H5I_INVALID_HID;
      }

      vtkDebugWithObjectMacro(this->Writer,
        << "Build mapping of " << name << " from [" << sourceOffset[0] << "+" << mappingSize[0]
        << "] to [" << destinationOffset[0] << "+" << mappingSize[0] << "]");

      // Create mapping H5S and select Hyperslab
      vtkHDF::ScopedH5SHandle mappedDataSpace =
        H5Screate_simple(numDim, mappingSize.data(), nullptr);
      if (mappedDataSpace == H5I_INVALID_HID)
      {
        return H5I_INVALID_HID;
      }
      if (H5Sselect_hyperslab(mappedDataSpace, H5S_SELECT_SET, sourceOffset.data(), nullptr,
            mappingSize.data(), nullptr) < 0)
      {
        return H5I_INVALID_HID;
      }

      // Build the mapping
      if (H5Pset_virtual(virtualSourceP, destSpace, this->SubfileNames[part].c_str(),
            datasetPath.c_str(), mappedDataSpace) < 0)
      {
        return H5I_INVALID_HID;
      }

      mappingOffset += mappingSize[0];
      sourceOffsets[part] += mappingSize[0];
    }
  }

  // Create the virtual dataset using all the mappings
  vtkHDF::ScopedH5DHandle vdset =
    H5Dcreate(group, name, type, destSpace, H5P_DEFAULT, virtualSourceP, H5P_DEFAULT);
  return vdset;
}

//------------------------------------------------------------------------------
hsize_t vtkHDFWriter::Implementation::GetNumberOfCellsSubfile(
  std::size_t subfileId, hsize_t part, bool isPolyData, const std::string& groupName)
{
  if (!isPolyData)
  {
    return this->GetSubfileNumberOf(PATH::NUMBER_OF_CELLS, subfileId, part);
  }

  if (isPolyData && groupName == PATH::CELL_DATA)
  {
    // Sum up the number of cells for each primitive type
    return std::accumulate(PrimitiveNames.begin(), PrimitiveNames.end(), 0,
      [this, subfileId, part](int sum, std::string prim) {
        return sum +
          this->GetSubfileNumberOf("/VTKHDF/" + prim + "/NumberOfCells", subfileId, part);
      });
  }
  else
  {
    return this->GetSubfileNumberOf(groupName + "/NumberOfCells", subfileId, part);
  }
}

//------------------------------------------------------------------------------
char vtkHDFWriter::Implementation::GetPrimitive(hid_t group)
{
  char primitive = 0;
  std::string groupName = this->GetGroupName(group);
  for (const auto& primitiveName : this->PrimitiveNames)
  {
    if (groupName == "/VTKHDF/" + primitiveName)
    {
      return primitive;
    }
    primitive++;
  }
  return -1;
}
//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::WriteSumSteps(hid_t group, const char* name)
{
  vtkDebugWithObjectMacro(
    this->Writer, "Creating steps sum " << name << " in " << this->GetGroupName(group));

  const std::string datasetPath = this->GetGroupName(group) + "/" + name;
  vtkHDF::ScopedH5DHandle dataset = this->OpenDataset(group, name);
  if (dataset == H5I_INVALID_HID)
  {
    return false;
  }

  // For each timestep, collect the sum of values in subfiles,
  // and append it to the meta-file array.
  for (int step = 0; step < this->Writer->NumberOfTimeSteps; step++)
  {
    int totalForTimeStep = 0;
    for (std::size_t part = 0; part < this->Subfiles.size(); part++)
    {
      totalForTimeStep += this->GetSubfileNumberOf(datasetPath, part, step);
    }

    if (!this->AddSingleValueToDataset(dataset, totalForTimeStep, false, false))
    {
      return false;
    }
  }

  return true;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::WriteSumStepsPolyData(hid_t group, const char* name)
{
  vtkDebugWithObjectMacro(
    this->Writer, "Creating polydata steps sum " << name << " in " << this->GetGroupName(group));

  const std::string datasetPath = this->GetGroupName(group) + "/" + name;
  vtkHDF::ScopedH5DHandle dataset = this->OpenDataset(group, name);
  if (dataset == H5I_INVALID_HID)
  {
    return false;
  }

  // Create VTK Array of size nbPrimitives * nbTimeSteps
  vtkNew<vtkIdTypeArray> totalsArray;
  totalsArray->SetNumberOfComponents(static_cast<int>(this->PrimitiveNames.size()));
  totalsArray->SetNumberOfTuples(this->Writer->NumberOfTimeSteps);

  // For each timestep, sum each primitive from all pieces
  for (int step = 0; step < this->Writer->NumberOfTimeSteps; step++)
  {
    totalsArray->SetTuple4(step, 0, 0, 0, 0);
    vtkDebugWithObjectMacro(this->Writer, "timestep " << step);
    for (int prim = 0; prim < static_cast<int>(this->PrimitiveNames.size()); prim++)
    {
      vtkDebugWithObjectMacro(this->Writer, "primitive " << prim);
      // Collect size for the current time step in each subfile for each primitive
      for (std::size_t part = 0; part < this->Subfiles.size(); part++)
      {
        auto current = totalsArray->GetComponent(step, prim);
        vtkDebugWithObjectMacro(this->Writer, "part " << part << " value " << current);
        totalsArray->SetComponent(
          step, prim, current + this->GetSubfileNumberOf(datasetPath, part, step, prim));
      }
    }
  }

  if (!this->AddArrayToDataset(dataset, totalsArray))
  {
    return false;
  }

  return true;
}

//------------------------------------------------------------------------------
hsize_t vtkHDFWriter::Implementation::GetSubfileNumberOf(
  const std::string& qualifier, std::size_t subfileId, hsize_t part, char primitive)
{
  vtkDebugWithObjectMacro(this->Writer, << "Fetching " << qualifier << " for subfile " << subfileId
                                        << " for part " << part << " with primitive "
                                        << static_cast<int>(primitive));

  vtkHDF::ScopedH5DHandle sourceDataset =
    H5Dopen(this->Subfiles[subfileId], qualifier.c_str(), H5P_DEFAULT);
  if (sourceDataset == H5I_INVALID_HID)
  {
    return 0;
  }

  std::vector<hsize_t> start{ static_cast<unsigned long>(part) }, count{ 1 }, result{ 0 };
  int dimension = 1;
  if (this->HdfType == "PolyData" && primitive != -1)
  {
    start.emplace_back(static_cast<hsize_t>(primitive));
    count.emplace_back(1);
    dimension++;
  }

  vtkHDF::ScopedH5SHandle sourceSpace = H5Dget_space(sourceDataset);
  vtkHDF::ScopedH5SHandle destSpace = H5Screate_simple(dimension, count.data(), nullptr);
  if (sourceSpace == H5I_INVALID_HID || sourceSpace == H5I_INVALID_HID)
  {
    return 0;
  }

  if (H5Sselect_hyperslab(
        sourceSpace, H5S_SELECT_SET, start.data(), nullptr, count.data(), nullptr) < 0)
  {
    return 0;
  }

  if (H5Dread(sourceDataset, H5T_NATIVE_INT, destSpace, sourceSpace, H5P_DEFAULT, result.data()) <
    0)
  {
    return 0;
  }

  return result[0];
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::GetSubFilesDatasetSize(
  const char* datasetPath, const char* groupName, hsize_t& totalSize)
{
  totalSize = 0;
  for (auto& fileRoot : this->Subfiles)
  {
    if (!H5Lexists(fileRoot, groupName, H5P_DEFAULT))
    {
      return false;
    }
    if (!H5Lexists(fileRoot, datasetPath, H5P_DEFAULT))
    {
      return false;
    }
    vtkHDF::ScopedH5DHandle sourceDataset = H5Dopen(fileRoot, datasetPath, H5P_DEFAULT);
    vtkHDF::ScopedH5SHandle sourceDataSpace = H5Dget_space(sourceDataset);
    std::vector<hsize_t> sourceDims(3);
    if (H5Sget_simple_extent_dims(sourceDataSpace, sourceDims.data(), nullptr) < 0)
    {
      return false;
    }

    totalSize += sourceDims[0];
  }
  return true;
}

//------------------------------------------------------------------------------
vtkHDFWriter::Implementation::IndexingMode vtkHDFWriter::Implementation::GetDatasetIndexationMode(
  hid_t group, const char* name)
{
  const std::string datasetPath = this->GetGroupName(group) + "/" + name;

  if (std::find(PATH::SINGLE_VALUES.begin(), PATH::SINGLE_VALUES.end(), name) !=
    PATH::SINGLE_VALUES.end())
  {
    return IndexingMode::MetaData;
  }
  if (this->GetGroupName(group) == PATH::POINT_DATA || name == PATH::POINTS)
  {
    return IndexingMode::Points;
  }
  if (this->GetGroupName(group) == PATH::CELL_DATA || name == PATH::OFFSETS || name == PATH::TYPES)
  {
    return IndexingMode::Cells;
  }
  if (name == PATH::CONNECTIVITY)
  {
    return IndexingMode::Connectivity;
  }
  return IndexingMode::Undefined;
}

//------------------------------------------------------------------------------
bool vtkHDFWriter::Implementation::InitDynamicDataset(hid_t group, const char* name, hid_t type,
  hsize_t cols, hsize_t chunkSize[], int compressionLevel)
{
  // When writing data externally, don't create a dynamic dataset,
  // But create a virtual one based on the subfiles on the last step or partition.
  if (!this->Subfiles.empty() && group != this->StepsGroup)
  {
    return true;
  }

  vtkHDF::ScopedH5SHandle emptyDataspace = this->CreateUnlimitedSimpleDataspace(cols);
  if (emptyDataspace == H5I_INVALID_HID)
  {
    return false;
  }
  vtkHDF::ScopedH5DHandle dataset = this->CreateChunkedHdfDataset(
    group, name, type, emptyDataspace, cols, chunkSize, compressionLevel);
  return dataset != H5I_INVALID_HID;
}

//------------------------------------------------------------------------------
vtkHDFWriter::Implementation::Implementation(vtkHDFWriter* writer)
  : Writer(writer)
  , File(H5I_INVALID_HID)
  , Root(H5I_INVALID_HID)
{
}

//------------------------------------------------------------------------------
vtkHDFWriter::Implementation::~Implementation() = default;

VTK_ABI_NAMESPACE_END
