/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkProbeLineFilter.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkProbeLineFilter.h"

#include "vtkAggregateDataSetFilter.h"
#include "vtkAppendArcLength.h"
#include "vtkAppendDataSets.h"
#include "vtkCell.h"
#include "vtkCellCenters.h"
#include "vtkCellData.h"
#include "vtkCellIterator.h"
#include "vtkCellLocatorStrategy.h"
#include "vtkCharArray.h"
#include "vtkCompositeDataSet.h"
#include "vtkCutter.h"
#include "vtkDIYExplicitAssigner.h"
#include "vtkDIYUtilities.h"
#include "vtkDataArrayRange.h"
#include "vtkDataObject.h"
#include "vtkDataSet.h"
#include "vtkDoubleArray.h"
#include "vtkFindCellStrategy.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkLineSource.h"
#include "vtkMath.h"
#include "vtkMathUtilities.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkMultiProcessController.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkPHyperTreeGridProbeFilter.h"
#include "vtkPProbeFilter.h"
#include "vtkPlane.h"
#include "vtkPointData.h"
#include "vtkPointSet.h"
#include "vtkPoints.h"
#include "vtkPolyData.h"
#include "vtkPolyDataAlgorithm.h"
#include "vtkPolyLineSource.h"
#include "vtkSMPTools.h"
#include "vtkSmartPointer.h"
#include "vtkStaticCellLocator.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkStringArray.h"
#include "vtkStripper.h"
#include "vtkVectorOperators.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>
#include <vector>

//------------------------------------------------------------------------------
vtkStandardNewMacro(vtkProbeLineFilter);

//------------------------------------------------------------------------------
vtkCxxSetObjectMacro(vtkProbeLineFilter, Controller, vtkMultiProcessController);

//------------------------------------------------------------------------------
namespace
{
void FillDefaultValues(vtkAbstractArray* array, double defaultValue = 0.0)
{
  if (auto* strArray = vtkStringArray::SafeDownCast(array))
  {
    vtkSMPTools::For(0, strArray->GetNumberOfValues(), [strArray](vtkIdType start, vtkIdType end) {
      for (vtkIdType i = start; i < end; ++i)
      {
        strArray->SetValue(i, "");
      }
    });
  }
  else if (auto* doubleArray = vtkDoubleArray::SafeDownCast(array))
  {
    auto range = vtk::DataArrayValueRange(doubleArray);
    vtkSMPTools::Fill(range.begin(), range.end(), vtkMath::Nan());
  }
  else if (auto* dataArray = vtkDataArray::SafeDownCast(array))
  {
    auto range = vtk::DataArrayValueRange(dataArray);
    vtkSMPTools::Fill(range.begin(), range.end(), defaultValue);
  }
}
};

//------------------------------------------------------------------------------
class vtkRemoteProbeLineMerger : public vtkPolyDataAlgorithm
{
public:
  static vtkRemoteProbeLineMerger* New();
  vtkTypeMacro(vtkRemoteProbeLineMerger, vtkPolyDataAlgorithm);

  vtkVector3d P1 = { 0.0, 0.0, 0.0 };
  vtkVector3d P2 = { 0.0, 0.0, 0.0 };
  double Tolerance = std::numeric_limits<double>::epsilon();
  bool SegmentCenters = false;

protected:
  struct MergeIndex
  {
    short DsIndex;
    vtkIdType PtIndex;
  };

  int FillInputPortInformation(int port, vtkInformation* info) override
  {
    if (port == 0)
    {
      info->Set(vtkAlgorithm::INPUT_IS_REPEATABLE(), 1);
      return this->Superclass::FillInputPortInformation(port, info);
    }
    return 0;
  }

  // Requirements :
  // - input is a point cloud without cells (if there are any they will be ignored)
  // - number of point is a multiple of 2
  // - input point data should have an array "arc_length"
  // - points are stored in from smaller to larger "arc_length" values
  // This filter will merge all data attributes, filling them with default values
  // if they are not merged by all inputs. It will then create the good line cells
  // according to the SegmentCenters boolean.
  int RequestData(vtkInformation* vtkNotUsed(request), vtkInformationVector** inputVector,
    vtkInformationVector* outputVector) override
  {
    const double lineDistance =
      std::sqrt(vtkMath::Distance2BetweenPoints(this->P1.GetData(), this->P2.GetData()));
    vtkInformation* outInfo = outputVector->GetInformationObject(0);
    vtkPolyData* output = vtkPolyData::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

    // Iterate on all inputs to :
    // - construct the array of valid inputs
    // - compute the total number of input points
    // - construct the structure of point data
    size_t totalNumberOfPoints = 0;
    std::vector<vtkPolyData*> inputs;
    std::vector<vtkDoubleArray*> lengthArrays;
    vtkNew<vtkPointData> outputPD;
    for (int cc = 0; cc < inputVector[0]->GetNumberOfInformationObjects(); cc++)
    {
      vtkPolyData* input = vtkPolyData::GetData(inputVector[0], cc);
      auto* inputPd = input->GetPointData();
      auto* arclength = vtkDoubleArray::SafeDownCast(inputPd->GetArray("arc_length"));
      if (input->GetNumberOfPoints() == 0 || !arclength)
      {
        continue;
      }

      totalNumberOfPoints += input->GetNumberOfPoints();
      inputs.push_back(input);
      lengthArrays.push_back(arclength);

      for (int i = 0; i < inputPd->GetNumberOfArrays(); ++i)
      {
        auto* inputArray = inputPd->GetAbstractArray(i);
        const char* name = inputArray->GetName();
        if (inputArray != arclength && !outputPD->GetAbstractArray(name))
        {
          auto newArray = vtk::TakeSmartPointer(inputArray->NewInstance());
          newArray->SetName(name);
          newArray->SetNumberOfComponents(inputArray->GetNumberOfComponents());
          outputPD->AddArray(newArray);
        }
      }
    }

    if (inputs.empty())
    {
      vtkNew<vtkPoints> outputPoints;
      outputPoints->InsertNextPoint(this->P1.GetData());
      outputPoints->InsertNextPoint(this->P2.GetData());
      vtkNew<vtkCharArray> validPointMask;
      validPointMask->SetName("vtkValidPointMask");
      validPointMask->InsertNextValue(0);
      validPointMask->InsertNextValue(0);
      vtkNew<vtkDoubleArray> arclength;
      arclength->SetName("arc_length");
      arclength->InsertNextValue(0.0);
      arclength->InsertNextValue(lineDistance);
      vtkNew<vtkPolyLineSource> polylineSource;
      polylineSource->SetPoints(outputPoints);
      polylineSource->Update();
      output->ShallowCopy(polylineSource->GetOutput());
      outputPD->AddArray(arclength);
      outputPD->AddArray(validPointMask);
      output->GetPointData()->ShallowCopy(outputPD);
      return 1;
    }

    // Construct the structure that will allow to merge inputs later on. We consider that
    // about 1 intersections out of 20 is followed by emptiness ie no cell is intersected
    // between the current cell and the next. This case should be fairely rare, but using
    // this heurisitc can greatly improve performance when working with very large datasets.
    std::vector<MergeIndex> mergedInputIndices;
    mergedInputIndices.reserve(static_cast<size_t>(1.05 * totalNumberOfPoints));
    std::vector<vtkIdType> pointIndices(inputs.size(), 0);
    double previousDistP2 = 0.0;
    while (true)
    {
      double distP1 = std::numeric_limits<double>::max();
      double distP2;
      short dsIndex = -1;
      for (size_t ds = 0; ds < inputs.size(); ++ds)
      {
        if (pointIndices[ds] >= inputs[ds]->GetNumberOfPoints())
        {
          continue;
        }

        vtkDoubleArray* arclength = lengthArrays[ds];
        double currentDistP1 = arclength->GetTuple1(pointIndices[ds]);
        if (currentDistP1 < distP1)
        {
          distP2 = arclength->GetTuple1(pointIndices[ds] + 1);
          dsIndex = ds;
          distP1 = currentDistP1;
        }
      }

      // If dsIndex is negative then it means we have processed all points and we can exit
      if (dsIndex < 0)
      {
        if (!vtkMathUtilities::NearlyEqual(previousDistP2, lineDistance, this->Tolerance))
        {
          mergedInputIndices.emplace_back(MergeIndex{ -1, -1 });
        }
        break;
      }

      // If current dist is not the same as previous distance then that means there is an
      // empty space between these 2 intersections.
      if (!vtkMathUtilities::NearlyEqual(distP1, previousDistP2, this->Tolerance))
      {
        mergedInputIndices.emplace_back(MergeIndex{ -1, -1 });
      }

      mergedInputIndices.emplace_back(MergeIndex{ dsIndex, pointIndices[dsIndex] });
      pointIndices[dsIndex] += 2;
      previousDistP2 = distP2;
    }

    // Resize all output structure to the correct number of points
    const size_t outputSize =
      this->SegmentCenters ? mergedInputIndices.size() + 2 : mergedInputIndices.size() * 2;
    vtkNew<vtkPoints> outputPoints;
    outputPoints->SetNumberOfPoints(outputSize);
    vtkNew<vtkCharArray> validPointMask;
    validPointMask->SetName("vtkValidPointMask");
    validPointMask->SetNumberOfTuples(outputSize);
    ::FillDefaultValues(validPointMask, 1);
    vtkNew<vtkDoubleArray> arclength;
    arclength->SetName("arc_length");
    arclength->SetNumberOfComponents(1);
    arclength->SetNumberOfTuples(outputSize);
    for (int i = 0; i < outputPD->GetNumberOfArrays(); ++i)
    {
      auto* array = outputPD->GetAbstractArray(i);
      array->SetNumberOfTuples(outputSize);
      ::FillDefaultValues(array);
    }

    // Merge all valid inputs
    if (this->SegmentCenters)
    {
      this->MergeSegmentCenters(inputs, mergedInputIndices, lengthArrays, outputPoints, outputPD,
        validPointMask, arclength);
    }
    else
    {
      this->MergeCellBoundaries(inputs, mergedInputIndices, lengthArrays, outputPoints, outputPD,
        validPointMask, arclength);
    }
    vtkNew<vtkPolyLineSource> polylineSource;
    polylineSource->SetPoints(outputPoints);
    polylineSource->Update();
    output->ShallowCopy(polylineSource->GetOutput());
    outputPD->AddArray(arclength);
    outputPD->AddArray(validPointMask);
    output->GetPointData()->ShallowCopy(outputPD);

    return 1;
  }

  void MergeCellBoundaries(const std::vector<vtkPolyData*>& inputs,
    const std::vector<MergeIndex>& mergedInputIndices,
    const std::vector<vtkDoubleArray*>& lengthArrays, vtkPoints* outputPoints,
    vtkPointData* outputPD, vtkCharArray* validPointMask, vtkDoubleArray* arclength)
  {
    vtkIdType startFor = 0;
    vtkIdType endFor = mergedInputIndices.size();
    if (mergedInputIndices[0].DsIndex < 0)
    {
      ++startFor;
      const auto& nextInfo = mergedInputIndices[1];
      outputPoints->SetPoint(0, this->P1.GetData());
      double tmpPnt[3];
      inputs[nextInfo.DsIndex]->GetPoint(nextInfo.PtIndex, tmpPnt);
      outputPoints->SetPoint(1, tmpPnt);
      validPointMask->SetValue(0, 0);
      validPointMask->SetValue(1, 0);
      arclength->SetValue(0, 0.0);
      arclength->SetValue(1, lengthArrays[nextInfo.DsIndex]->GetValue(nextInfo.PtIndex));
    }
    if (mergedInputIndices.back().DsIndex < 0)
    {
      --endFor;
      const vtkIdType idx = endFor * 2;
      const auto& previousInfo = mergedInputIndices[endFor - 1];
      double tmpPnt[3];
      inputs[previousInfo.DsIndex]->GetPoint(previousInfo.PtIndex + 1, tmpPnt);
      outputPoints->SetPoint(idx, tmpPnt);
      outputPoints->SetPoint(idx + 1, this->P2.GetData());
      validPointMask->SetValue(idx, 0);
      validPointMask->SetValue(idx + 1, 0);
      arclength->SetValue(
        idx, lengthArrays[previousInfo.DsIndex]->GetValue(previousInfo.PtIndex + 1));
      arclength->SetValue(idx + 1,
        std::sqrt(vtkMath::Distance2BetweenPoints(this->P1.GetData(), this->P2.GetData())));
    }
    vtkSMPTools::For(startFor, endFor, [&](vtkIdType start, vtkIdType end) {
      for (vtkIdType pt = start; pt < end; ++pt)
      {
        const vtkIdType idx = pt * 2;
        const auto& mergeInfo = mergedInputIndices[pt];
        if (mergeInfo.DsIndex < 0)
        {
          const auto& previousInfo = mergedInputIndices[pt - 1];
          const auto& nextInfo = mergedInputIndices[pt + 1];
          double tmpPnt[3];
          inputs[previousInfo.DsIndex]->GetPoint(previousInfo.PtIndex + 1, tmpPnt);
          outputPoints->SetPoint(idx, tmpPnt);
          inputs[nextInfo.DsIndex]->GetPoint(nextInfo.PtIndex, tmpPnt);
          outputPoints->SetPoint(idx + 1, tmpPnt);
          validPointMask->SetValue(idx, 0);
          validPointMask->SetValue(idx + 1, 0);
          arclength->SetValue(
            idx, lengthArrays[previousInfo.DsIndex]->GetValue(previousInfo.PtIndex + 1));
          arclength->SetValue(idx + 1, lengthArrays[nextInfo.DsIndex]->GetValue(nextInfo.PtIndex));
        }
        else
        {
          vtkPolyData* ds = inputs[mergeInfo.DsIndex];
          double tmpPnt[3];
          ds->GetPoint(mergeInfo.PtIndex, tmpPnt);
          outputPoints->SetPoint(idx, tmpPnt);
          ds->GetPoint(mergeInfo.PtIndex + 1, tmpPnt);
          outputPoints->SetPoint(idx + 1, tmpPnt);
          vtkDoubleArray* lengths = lengthArrays[mergeInfo.DsIndex];
          arclength->SetValue(idx, lengths->GetValue(mergeInfo.PtIndex));
          arclength->SetValue(idx + 1, lengths->GetValue(mergeInfo.PtIndex + 1));
          for (int i = 0; i < outputPD->GetNumberOfArrays(); ++i)
          {
            auto* targetArray = outputPD->GetAbstractArray(i);
            auto* sourceArray = ds->GetPointData()->GetAbstractArray(targetArray->GetName());
            targetArray->SetTuple(idx, mergeInfo.PtIndex, sourceArray);
            targetArray->SetTuple(idx + 1, mergeInfo.PtIndex + 1, sourceArray);
          }
        }
      }
    });
  }

  void MergeSegmentCenters(const std::vector<vtkPolyData*>& inputs,
    const std::vector<MergeIndex>& mergedInputIndices,
    const std::vector<vtkDoubleArray*>& lengthArrays, vtkPoints* outputPoints,
    vtkPointData* outputPD, vtkCharArray* validPointMask, vtkDoubleArray* arclength)
  {
    vtkIdType startFor = 0;
    vtkIdType endFor = mergedInputIndices.size();
    outputPoints->SetPoint(0, this->P1.GetData());
    arclength->SetValue(0, 0.0);
    if (mergedInputIndices[0].DsIndex < 0)
    {
      const auto& nextInfo = mergedInputIndices[1];
      vtkVector3d pnt;
      inputs[nextInfo.DsIndex]->GetPoint(nextInfo.PtIndex, pnt.GetData());
      pnt = (pnt + this->P1) * 0.5;
      outputPoints->SetPoint(1, pnt.GetData());
      validPointMask->SetValue(0, 0);
      validPointMask->SetValue(1, 0);
      arclength->SetValue(1, lengthArrays[nextInfo.DsIndex]->GetValue(nextInfo.PtIndex) * 0.5);
      ++startFor;
    }
    else
    {
      const auto& mergeInfo = mergedInputIndices[0];
      vtkPolyData* ds = inputs[mergeInfo.DsIndex];
      for (int i = 0; i < outputPD->GetNumberOfArrays(); ++i)
      {
        auto* targetArray = outputPD->GetAbstractArray(i);
        auto* sourceArray = ds->GetPointData()->GetAbstractArray(targetArray->GetName());
        targetArray->SetTuple(0, mergeInfo.PtIndex, sourceArray);
      }
    }
    outputPoints->SetPoint(endFor + 1, this->P2.GetData());
    arclength->SetValue(endFor + 1,
      std::sqrt(vtkMath::Distance2BetweenPoints(this->P1.GetData(), this->P2.GetData())));
    if (mergedInputIndices.back().DsIndex < 0)
    {
      const auto& previousInfo = mergedInputIndices[endFor - 2];
      vtkVector3d pnt;
      inputs[previousInfo.DsIndex]->GetPoint(previousInfo.PtIndex + 1, pnt.GetData());
      pnt = (pnt + this->P2) * 0.5;
      outputPoints->SetPoint(endFor, pnt.GetData());
      validPointMask->SetValue(endFor, 0);
      validPointMask->SetValue(endFor + 1, 0);
      arclength->SetValue(
        endFor, lengthArrays[previousInfo.DsIndex]->GetValue(previousInfo.PtIndex + 1));
      --endFor;
    }
    else
    {
      const auto& mergeInfo = mergedInputIndices.back();
      vtkPolyData* ds = inputs[mergeInfo.DsIndex];
      for (int i = 0; i < outputPD->GetNumberOfArrays(); ++i)
      {
        auto* targetArray = outputPD->GetAbstractArray(i);
        auto* sourceArray = ds->GetPointData()->GetAbstractArray(targetArray->GetName());
        targetArray->SetTuple(endFor + 1, mergeInfo.PtIndex + 1, sourceArray);
      }
    }
    vtkSMPTools::For(startFor, endFor, [&](vtkIdType start, vtkIdType end) {
      vtkNew<vtkIdList> interpList;
      interpList->SetNumberOfIds(2);
      for (vtkIdType pt = start; pt < end; ++pt)
      {
        const vtkIdType idx = pt + 1;
        const auto& mergeInfo = mergedInputIndices[pt];
        if (mergeInfo.DsIndex < 0)
        {
          const auto& previousInfo = mergedInputIndices[pt - 1];
          const auto& nextInfo = mergedInputIndices[pt + 1];
          vtkVector3d pnt1, pnt2;
          inputs[previousInfo.DsIndex]->GetPoint(previousInfo.PtIndex + 1, pnt1.GetData());
          inputs[nextInfo.DsIndex]->GetPoint(nextInfo.PtIndex, pnt2.GetData());
          pnt1 = (pnt1 + pnt2) * 0.5;
          outputPoints->SetPoint(idx, pnt1.GetData());
          validPointMask->SetValue(idx, 0);
          double length = lengthArrays[previousInfo.DsIndex]->GetValue(previousInfo.PtIndex + 1) +
            lengthArrays[nextInfo.DsIndex]->GetValue(previousInfo.PtIndex);
          arclength->SetValue(idx, length * 0.5);
        }
        else
        {
          vtkPolyData* ds = inputs[mergeInfo.DsIndex];
          vtkVector3d pnt1, pnt2;
          ds->GetPoint(mergeInfo.PtIndex, pnt1.GetData());
          ds->GetPoint(mergeInfo.PtIndex + 1, pnt2.GetData());
          pnt1 = (pnt1 + pnt2) * 0.5;
          outputPoints->SetPoint(idx, pnt1.GetData());

          vtkDoubleArray* lengths = lengthArrays[mergeInfo.DsIndex];
          double length =
            lengths->GetValue(mergeInfo.PtIndex) + lengths->GetValue(mergeInfo.PtIndex + 1);
          arclength->SetValue(idx, length * 0.5);
          for (int i = 0; i < outputPD->GetNumberOfArrays(); ++i)
          {
            auto* targetArray = outputPD->GetAbstractArray(i);
            auto* sourceArray = ds->GetPointData()->GetAbstractArray(targetArray->GetName());
            interpList->SetId(0, mergeInfo.PtIndex);
            interpList->SetId(1, mergeInfo.PtIndex + 1);
            double weights[2] = { 0.5, 0.5 };
            targetArray->InterpolateTuple(idx, interpList, sourceArray, weights);
          }
        }
      }
    });
  }
};
vtkStandardNewMacro(vtkRemoteProbeLineMerger);

//------------------------------------------------------------------------------
class vtkLocalProbeLineMerger : public vtkPolyDataAlgorithm
{
public:
  static vtkLocalProbeLineMerger* New();
  vtkTypeMacro(vtkLocalProbeLineMerger, vtkPolyDataAlgorithm);

protected:
  struct MergeIndex
  {
    short DsIndex;
    vtkIdType PtIndex;
  };

  int FillInputPortInformation(int port, vtkInformation* info) override
  {
    if (port == 0)
    {
      info->Set(vtkAlgorithm::INPUT_IS_REPEATABLE(), 1);
      return this->Superclass::FillInputPortInformation(port, info);
    }
    return 0;
  }

  // Requirements :
  // - input is a point cloud without cells (if there are any they will be ignored)
  // - number of point is a multiple of 2
  // - string attributes should have a single component
  // - input point data should have an array "arc_length"
  // This filter will merge all data attributes, filling them with default values
  // if they are not merged by all inputs. It will not create any cells but only sort
  // points in the good order, so it smaller to transfer over MPI processes later on.
  int RequestData(vtkInformation* vtkNotUsed(request), vtkInformationVector** inputVector,
    vtkInformationVector* outputVector) override
  {
    vtkInformation* outInfo = outputVector->GetInformationObject(0);
    vtkPolyData* output = vtkPolyData::SafeDownCast(outInfo->Get(vtkDataObject::DATA_OBJECT()));

    vtkIdType totalNumberOfPoints = 0;
    std::vector<vtkPolyData*> inputs;
    std::vector<vtkDoubleArray*> lengthArrays;
    vtkNew<vtkPointData> outputPD;
    for (int cc = 0; cc < inputVector[0]->GetNumberOfInformationObjects(); cc++)
    {
      vtkPolyData* input = vtkPolyData::GetData(inputVector[0], cc);
      auto* inputPd = input->GetPointData();
      auto* arclength = vtkDoubleArray::SafeDownCast(inputPd->GetArray("arc_length"));
      if (input->GetNumberOfPoints() == 0 || !arclength)
      {
        continue;
      }

      totalNumberOfPoints += input->GetNumberOfPoints();
      inputs.push_back(input);
      lengthArrays.push_back(arclength);

      for (int i = 0; i < inputPd->GetNumberOfArrays(); ++i)
      {
        auto* inputArray = inputPd->GetAbstractArray(i);
        const char* name = inputArray->GetName();
        if (inputArray != arclength && !outputPD->GetAbstractArray(name))
        {
          auto newArray = vtk::TakeSmartPointer(inputArray->NewInstance());
          newArray->SetName(name);
          newArray->SetNumberOfComponents(inputArray->GetNumberOfComponents());
          outputPD->AddArray(newArray);
        }
      }
    }

    if (inputs.empty())
    {
      return 1;
    }
    else if (inputs.size() == 1)
    {
      output->ShallowCopy(inputs[0]);
      return 1;
    }

    // Resize all output structure to the correct number of points
    vtkNew<vtkPoints> outputPoints;
    outputPoints->SetNumberOfPoints(totalNumberOfPoints);
    vtkNew<vtkCharArray> validPointMask;
    validPointMask->SetName("vtkValidPointMask");
    validPointMask->SetNumberOfTuples(totalNumberOfPoints);
    ::FillDefaultValues(validPointMask, 1);
    vtkNew<vtkDoubleArray> arclength;
    arclength->SetName("arc_length");
    arclength->SetNumberOfComponents(1);
    arclength->SetNumberOfTuples(totalNumberOfPoints);
    for (int i = 0; i < outputPD->GetNumberOfArrays(); ++i)
    {
      auto* array = outputPD->GetAbstractArray(i);
      array->SetNumberOfTuples(totalNumberOfPoints);
      ::FillDefaultValues(array);
    }

    // Merge all valid inputs
    std::vector<vtkIdType> inputIdx(inputs.size(), 0);
    for (vtkIdType outputIdx = 0; outputIdx < totalNumberOfPoints; outputIdx += 2)
    {
      double minDist = std::numeric_limits<double>::max();
      short dsIndex = -1;
      for (size_t ds = 0; ds < inputs.size(); ++ds)
      {
        if (inputIdx[ds] >= inputs[ds]->GetNumberOfPoints())
        {
          continue;
        }

        double currentDist = lengthArrays[ds]->GetTuple1(inputIdx[ds]);
        if (currentDist < minDist)
        {
          dsIndex = ds;
          minDist = currentDist;
        }
      }

      if (dsIndex < 0)
      {
        break;
      }

      vtkPolyData* ds = inputs[dsIndex];
      vtkIdType sourceIndex = inputIdx[dsIndex];
      double tmpPnt[3];
      ds->GetPoint(sourceIndex, tmpPnt);
      outputPoints->SetPoint(outputIdx, tmpPnt);
      ds->GetPoint(sourceIndex + 1, tmpPnt);
      outputPoints->SetPoint(outputIdx + 1, tmpPnt);
      vtkDoubleArray* lengths = lengthArrays[dsIndex];
      arclength->SetValue(outputIdx, lengths->GetValue(sourceIndex));
      arclength->SetValue(outputIdx + 1, lengths->GetValue(sourceIndex + 1));
      for (int i = 0; i < outputPD->GetNumberOfArrays(); ++i)
      {
        auto* targetArray = outputPD->GetAbstractArray(i);
        auto* sourceArray = ds->GetPointData()->GetAbstractArray(targetArray->GetName());
        targetArray->SetTuple(outputIdx, sourceIndex, sourceArray);
        targetArray->SetTuple(outputIdx + 1, sourceIndex + 1, sourceArray);
      }
      inputIdx[dsIndex] += 2;
    }

    // Construct final dataset
    outputPD->AddArray(arclength);
    outputPD->AddArray(validPointMask);
    output->SetPoints(outputPoints);
    output->GetPointData()->ShallowCopy(outputPD);

    return 1;
  }
};
vtkStandardNewMacro(vtkLocalProbeLineMerger);

//------------------------------------------------------------------------------
struct vtkProbeLineFilter::vtkInternals
{
  vtkMTimeType PreviousInputTime = 0;
  std::map<vtkDataSet*, vtkSmartPointer<vtkFindCellStrategy>> Strategies;

  void UpdateLocators(vtkDataObject* input, int pattern, const double tolerance)
  {
    vtkMTimeType inputTime = input->GetMTime();
    bool isInputDifferent = inputTime != this->PreviousInputTime;
    bool needLocators = pattern == vtkProbeLineFilter::SAMPLE_LINE_AT_CELL_BOUNDARIES ||
      pattern == vtkProbeLineFilter::SAMPLE_LINE_AT_SEGMENT_CENTERS;
    if (isInputDifferent && needLocators)
    {
      this->PreviousInputTime = inputTime;

      const auto& inputs = vtkCompositeDataSet::GetDataSets(input);
      for (vtkDataSet* ds : inputs)
      {
        if (!ds || !ds->GetNumberOfCells())
        {
          continue;
        }

        vtkNew<vtkStaticCellLocator> locator;
        locator->SetDataSet(ds);
        locator->SetTolerance(tolerance);
        locator->BuildLocator();

        vtkCellLocatorStrategy* strategy = vtkCellLocatorStrategy::New();
        strategy->SetCellLocator(locator);

        this->Strategies[ds] =
          vtkSmartPointer<vtkFindCellStrategy>::Take(static_cast<vtkFindCellStrategy*>(strategy));
      }
    }
  }
};

namespace
{
//==============================================================================
/**
 * Store the information of the intersection between a cell and a ray. InT and OutT
 * are the parametric distances on the ray for the first (and second for 3D cells)
 * intersection between the ray and the cell. CellId is the id of the intersected cell.
 * A value of -1 means that the intersection is happening outside the cell.
 */
struct HitCellInfo
{
  double InT = -1.0;
  double OutT = -1.0;
  std::array<double, 3> InPCoords = { 0.0, 0.0, 0.0 };
  std::array<double, 3> OutPCoords = { 0.0, 0.0, 0.0 };
  std::array<double, 3> InPos = { 0.0, 0.0, 0.0 };
  std::array<double, 3> OutPos = { 0.0, 0.0, 0.0 };
  vtkIdType CellId = -1;

  operator bool() const noexcept { return this->InT >= 0.0 && this->OutT >= 0.0; }

  bool operator<(const HitCellInfo& r) const noexcept { return this->InT < r.InT; }
};

//==============================================================================
/**
 * Return the entry point and exit point of a given cell for the segment [p1,p2].
 */
HitCellInfo GetInOutCell(const vtkVector3d& p1, const vtkVector3d& p2, vtkIdType cellId,
  vtkDataSet* input, double tolerance)
{
  HitCellInfo res;
  res.CellId = cellId;

  vtkCell* cell = input->GetCell(cellId);
  double t;
  int subId;
  if (cell->IntersectWithLine(
        p1.GetData(), p2.GetData(), tolerance, t, res.InPos.data(), res.InPCoords.data(), subId))
  {
    res.InT = t;
  }
  if (cell->IntersectWithLine(
        p2.GetData(), p1.GetData(), tolerance, t, res.OutPos.data(), res.OutPCoords.data(), subId))
  {
    res.OutT = 1.0 - t;
  }

  if (cell->GetCellDimension() == 3 && vtkMathUtilities::NearlyEqual(res.InT, res.OutT, tolerance))
  {
    res.InT = res.OutT = -1.0;
  }

  return res;
}
}

//------------------------------------------------------------------------------
vtkProbeLineFilter::vtkProbeLineFilter()
  : Internal(new vtkInternals)
{
  this->SetNumberOfInputPorts(2);
  this->SetController(vtkMultiProcessController::GetGlobalController());
}

//------------------------------------------------------------------------------
vtkProbeLineFilter::~vtkProbeLineFilter()
{
  this->SetController(nullptr);
  delete this->Internal;
}

//------------------------------------------------------------------------------
int vtkProbeLineFilter::RequestData(
  vtkInformation*, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  // Check inputs / outputs
  vtkInformation* inputInfo = inputVector[0]->GetInformationObject(0);
  vtkInformation* samplerInfo = inputVector[1]->GetInformationObject(0);
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  if (!outInfo || !inputInfo || !samplerInfo)
  {
    vtkErrorMacro("Missing input or output information");
    return 0;
  }

  vtkDataObject* input = inputInfo->Get(vtkDataObject::DATA_OBJECT());
  auto* samplerLocal = vtkPointSet::SafeDownCast(samplerInfo->Get(vtkDataObject::DATA_OBJECT()));
  auto* output = outInfo->Get(vtkDataObject::DATA_OBJECT());
  bool outputIsValid = this->AggregateAsPolyData
    ? vtkPolyData::SafeDownCast(output) != nullptr
    : vtkMultiBlockDataSet::SafeDownCast(output) != nullptr;
  if (!output || !input || !samplerLocal || !outputIsValid)
  {
    vtkErrorMacro("Missing input or output");
    return 0;
  }

  // The probe locations source need to be the same on all ranks : always take rank 0 source
  auto sampler = vtkSmartPointer<vtkPointSet>::Take(samplerLocal->NewInstance());
  if (this->Controller->GetLocalProcessId() == 0)
  {
    this->Controller->Broadcast(samplerLocal, 0);
    sampler->ShallowCopy(samplerLocal);
  }
  else
  {
    this->Controller->Broadcast(sampler, 0);
  }

  // Compute tolerance if needed
  double tolerance = this->Tolerance;
  if (this->ComputeTolerance)
  {
    double bounds[6] = { 0, 0, 0, 0, 0, 0 };
    if (auto cds = vtkCompositeDataSet::SafeDownCast(input))
    {
      cds->GetBounds(bounds);
    }
    else if (auto ds = vtkDataSet::SafeDownCast(input))
    {
      ds->GetBounds(bounds);
    }
    vtkBoundingBox bb(bounds);
    if (bb.IsValid())
    {
      tolerance = std::numeric_limits<float>::epsilon() * bb.GetDiagonalLength();
    }
  }
  this->Internal->UpdateLocators(input, this->SamplingPattern, tolerance);

  // For each cell create the line that probe all data and add it to the resulting multiblock
  auto samplerCellsIt = vtkSmartPointer<vtkCellIterator>::Take(sampler->NewCellIterator());
  vtkNew<vtkMultiBlockDataSet> multiBlockOutput;
  for (samplerCellsIt->InitTraversal(); !samplerCellsIt->IsDoneWithTraversal();
       samplerCellsIt->GoToNextCell())
  {
    if (samplerCellsIt->GetCellType() == VTK_LINE || samplerCellsIt->GetCellType() == VTK_POLY_LINE)
    {
      auto polyline = this->CreateSamplingPolyLine(
        sampler->GetPoints(), samplerCellsIt->GetPointIds(), input, tolerance);
      const unsigned int block = multiBlockOutput->GetNumberOfBlocks();
      multiBlockOutput->SetNumberOfBlocks(block + 1);
      multiBlockOutput->SetBlock(block, polyline);
    }
  }

  if (this->AggregateAsPolyData)
  {
    vtkNew<vtkAppendDataSets> appender;
    appender->SetMergePoints(false);
    appender->SetOutputDataSetType(VTK_POLY_DATA);
    for (unsigned int i = 0; i < multiBlockOutput->GetNumberOfBlocks(); ++i)
    {
      appender->AddInputData(multiBlockOutput->GetBlock(i));
    }
    appender->Update();
    output->ShallowCopy(appender->GetOutputDataObject(0));
  }
  else
  {
    output->ShallowCopy(multiBlockOutput);
  }

  return 1;
}

//------------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkProbeLineFilter::CreateSamplingPolyLine(
  vtkPoints* points, vtkIdList* pointIds, vtkDataObject* input, double tolerance) const
{
  std::vector<vtkSmartPointer<vtkPolyData>> polylines;
  double previousLength = 0.0;
  for (vtkIdType i = 0; i < pointIds->GetNumberOfIds() - 1; ++i)
  {
    const vtkVector3d p1(points->GetPoint(pointIds->GetId(i)));
    const vtkVector3d p2(points->GetPoint(pointIds->GetId(i + 1)));
    vtkSmartPointer<vtkPolyData> current = (this->SamplingPattern == SAMPLE_LINE_UNIFORMLY)
      ? this->SampleLineUniformly(p1, p2, input, tolerance)
      : this->SampleLineAtEachCell(p1, p2, input, tolerance);

    vtkDataArray* arclength = nullptr;
    if (current.Get() != nullptr && current->GetNumberOfPoints() > 1)
    {
      arclength = current->GetPointData()->GetArray("arc_length");
    }
    if (arclength && current->GetNumberOfCells() == 1)
    {
      if (previousLength != 0.0)
      {
        auto range = vtk::DataArrayValueRange<1>(arclength);
        vtkSMPTools::Transform(range.begin(), range.end(), range.begin(),
          [previousLength](double x) { return x + previousLength; });
      }
      previousLength = arclength->GetTuple1(arclength->GetNumberOfValues() - 1);
      polylines.emplace_back(current);
    }
  }

  if (polylines.empty())
  {
    return vtkSmartPointer<vtkPolyData>::New();
  }
  else if (polylines.size() == 1)
  {
    return polylines[0];
  }
  else
  {
    vtkNew<vtkAppendDataSets> appender;
    for (const auto& pl : polylines)
    {
      appender->AddInputData(0, pl);
    }
    appender->SetOutputDataSetType(VTK_POLY_DATA);
    appender->SetMergePoints(false);
    appender->Update();
    return appender->GetPolyDataOutput();
  }
}

//-----------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkProbeLineFilter::SampleLineUniformly(
  const vtkVector3d& p1, const vtkVector3d& p2, vtkDataObject* input, double tolerance) const
{
  vtkNew<vtkLineSource> lineSource;
  lineSource->SetPoint1(p1.GetData());
  lineSource->SetPoint2(p2.GetData());
  lineSource->SetResolution(this->LineResolution);
  lineSource->Update();

  vtkNew<vtkPProbeFilter> prober;
  prober->SetController(this->Controller);
  prober->SetPassPartialArrays(this->PassPartialArrays);
  prober->SetPassCellArrays(this->PassCellArrays);
  prober->SetPassPointArrays(this->PassPointArrays);
  prober->SetPassFieldArrays(this->PassFieldArrays);
  prober->SetComputeTolerance(this->ComputeTolerance);
  prober->SetTolerance(tolerance);
  prober->SetSourceData(input);
  prober->SetFindCellStrategyMap(this->Internal->Strategies);
  prober->SetInputData(lineSource->GetOutput());
  prober->Update();

  vtkNew<vtkAppendArcLength> arcs;
  arcs->SetInputConnection(prober->GetOutputPort());
  arcs->Update();

  return arcs->GetOutput();
}

//------------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkProbeLineFilter::SampleLineAtEachCell(
  const vtkVector3d& p1, const vtkVector3d& p2, vtkDataObject* input, const double tolerance) const
{
  std::vector<vtkDataSet*> inputs = vtkCompositeDataSet::GetDataSets(input);
  if (vtkMathUtilities::NearlyEqual(p1[0], p2[0], tolerance) &&
    vtkMathUtilities::NearlyEqual(p1[1], p2[1], tolerance) &&
    vtkMathUtilities::NearlyEqual(p1[2], p2[2], tolerance))
  {
    vtkNew<vtkLineSource> line;
    line->SetPoint1(p1.GetData());
    line->SetPoint2(p2.GetData());
    line->Update();
    return vtkPolyData::SafeDownCast(line->GetOutputDataObject(0));
  }

  // Add every intersection with all blocks of the dataset on our current rank.
  vtkNew<vtkLocalProbeLineMerger> localMerger;
  for (std::size_t dsId = 0; dsId < inputs.size(); ++dsId)
  {
    localMerger->AddInputData(0, this->IntersectCells(p1, p2, inputs[dsId], tolerance));
  }
  localMerger->Update();

  // Merge polyline from all MPI processes
  constexpr int MPI_COMMUNICATION_TAG = 2022;
  int procid = 0;
  int numProcs = 1;
  if (this->Controller)
  {
    procid = this->Controller->GetLocalProcessId();
    numProcs = this->Controller->GetNumberOfProcesses();
  }

  vtkNew<vtkRemoteProbeLineMerger> merger;
  merger->P1 = p1;
  merger->P2 = p2;
  merger->Tolerance = tolerance;
  merger->SegmentCenters = (this->SamplingPattern == SAMPLE_LINE_AT_SEGMENT_CENTERS);
  vtkSmartPointer<vtkPolyData> output;
  if (procid != 0)
  {
    // Satellite nodes send their local polyline
    this->Controller->Send(localMerger->GetOutput(), 0, MPI_COMMUNICATION_TAG);
  }
  else
  {
    merger->AddInputData(0, localMerger->GetOutput());

    for (int distId = 1; distId < numProcs; ++distId)
    {
      auto remotePolyline = vtkSmartPointer<vtkPolyData>::New();
      this->Controller->Receive(remotePolyline, distId, MPI_COMMUNICATION_TAG);
      merger->AddInputData(0, remotePolyline);
    }

    merger->Update();
    output = merger->GetOutput();
  }

  return output;
}

//------------------------------------------------------------------------------
vtkSmartPointer<vtkPolyData> vtkProbeLineFilter::IntersectCells(
  const vtkVector3d& p1, const vtkVector3d& p2, vtkDataSet* dataset, double tolerance) const
{
  auto result = vtkSmartPointer<vtkPolyData>::New();

  // Get all cell intersections for the current dataset. There is some cases where there
  // is no strategy associated to a dataset, usually because the dataset has 0 cells.
  // In this case `intersections` will stay empty.
  std::vector<HitCellInfo> intersections;
  auto* strategy = vtkCellLocatorStrategy::SafeDownCast(this->Internal->Strategies[dataset]);
  if (strategy)
  {
    vtkAbstractCellLocator* locator = strategy->GetCellLocator();
    vtkNew<vtkIdList> intersectedIds;
    locator->FindCellsAlongLine(p1.GetData(), p2.GetData(), 0.0, intersectedIds);
    for (vtkIdType i = 0; i < intersectedIds->GetNumberOfIds(); ++i)
    {
      vtkIdType cellId = intersectedIds->GetId(i);
      if (dataset->HasAnyGhostCells() && dataset->GetCellGhostArray()->GetValue(cellId))
      {
        continue;
      }

      auto inOut = ::GetInOutCell(p1, p2, cellId, dataset, tolerance);
      if (inOut)
      {
        intersections.emplace_back(inOut);
      }
    }
  }

  // Make sure our intersections are sorted
  vtkSMPTools::Sort(intersections.begin(), intersections.end());

  // Create a point cloud also storing point and cells data for these intersections
  vtkNew<vtkPoints> linePoints;
  linePoints->SetNumberOfPoints(intersections.size() * 2);
  vtkNew<vtkDoubleArray> arclengthArray;
  arclengthArray->SetName("arc_length");
  arclengthArray->SetNumberOfValues(linePoints->GetNumberOfPoints());
  const double lineLength = (p2 - p1).Norm();
  for (size_t i = 0; i < intersections.size(); ++i)
  {
    const vtkIdType idx = static_cast<vtkIdType>(i) * 2;
    linePoints->SetPoint(idx, intersections[i].InPos.data());
    arclengthArray->SetValue(idx, intersections[i].InT * lineLength);
    linePoints->SetPoint(idx + 1, intersections[i].OutPos.data());
    arclengthArray->SetValue(idx + 1, intersections[i].OutT * lineLength);
  }
  result->SetPoints(linePoints);

  auto lambdaAddAttribute = [&linePoints](
                              vtkAbstractArray* attribute, vtkDataSetAttributes* dsAttributes) {
    const char* name = attribute->GetName();
    vtkSmartPointer<vtkAbstractArray> targetArray;
    if (!dsAttributes->GetAbstractArray(name))
    {
      targetArray = vtk::TakeSmartPointer(attribute->NewInstance());
      targetArray->SetName(name);
      targetArray->SetNumberOfComponents(attribute->GetNumberOfComponents());
      targetArray->SetNumberOfTuples(linePoints->GetNumberOfPoints());
      dsAttributes->AddArray(targetArray);
    }
    return targetArray;
  };

  // Interpolate point data to intersection locations
  vtkPointData* resultPointData = result->GetPointData();
  resultPointData->AddArray(arclengthArray);
  auto* inputPointData = dataset->GetPointData();
  for (int i = 0; i < inputPointData->GetNumberOfArrays(); ++i)
  {
    vtkAbstractArray* sourceArray = inputPointData->GetAbstractArray(i);
    if (auto targetArray = lambdaAddAttribute(sourceArray, resultPointData))
    {
      for (size_t j = 0; j < intersections.size(); ++j)
      {
        const auto& inter = intersections[j];
        vtkCell* cell = dataset->GetCell(inter.CellId);

        double weights[cell->GetNumberOfPoints()];
        cell->InterpolateFunctions(inter.InPCoords.data(), weights);
        targetArray->InterpolateTuple(j * 2, cell->GetPointIds(), sourceArray, weights);

        cell->InterpolateFunctions(inter.OutPCoords.data(), weights);
        targetArray->InterpolateTuple(j * 2 + 1, cell->GetPointIds(), sourceArray, weights);
      }
    }
  }

  // Translate cell data to point data
  auto* inputCellData = dataset->GetCellData();
  for (int i = 0; i < inputCellData->GetNumberOfArrays(); ++i)
  {
    vtkAbstractArray* sourceArray = inputCellData->GetAbstractArray(i);
    if (auto targetArray = lambdaAddAttribute(sourceArray, resultPointData))
    {
      for (size_t j = 0; j < intersections.size(); ++j)
      {
        const auto& inter = intersections[j];
        targetArray->SetTuple(j * 2, inter.CellId, sourceArray);
        targetArray->SetTuple(j * 2 + 1, inter.CellId, sourceArray);
      }
    }
  }

  return result;
}

//------------------------------------------------------------------------------
int vtkProbeLineFilter::FillInputPortInformation(int port, vtkInformation* info)
{

  switch (port)
  {
    case 0:
      info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkDataSet");
      info->Append(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkCompositeDataSet");
      break;

    case 1:
      info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkPolyData");
      info->Append(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkUnstructuredGrid");
      break;

    default:
      break;
  }

  return 1;
}

//------------------------------------------------------------------------------
int vtkProbeLineFilter::RequestDataObject(vtkInformation* vtkNotUsed(request),
  vtkInformationVector** vtkNotUsed(inputVector), vtkInformationVector* outputVector)
{
  vtkInformation* outInfo = outputVector->GetInformationObject(0);
  if (this->AggregateAsPolyData)
  {
    vtkPolyData* output = vtkPolyData::GetData(outInfo);
    if (!output)
    {
      auto newOutput = vtkSmartPointer<vtkPolyData>::New();
      outInfo->Set(vtkDataObject::DATA_OBJECT(), newOutput);
    }
  }
  else
  {
    vtkMultiBlockDataSet* output = vtkMultiBlockDataSet::GetData(outInfo);
    if (!output)
    {
      auto newOutput = vtkSmartPointer<vtkMultiBlockDataSet>::New();
      outInfo->Set(vtkDataObject::DATA_OBJECT(), newOutput);
    }
  }

  return 1;
}

//------------------------------------------------------------------------------
void vtkProbeLineFilter::SetSourceConnection(vtkAlgorithmOutput* input)
{
  this->SetInputConnection(1, input);
}

//------------------------------------------------------------------------------
void vtkProbeLineFilter::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "Controller: " << this->Controller << endl;
  switch (this->SamplingPattern)
  {
    case SAMPLE_LINE_AT_CELL_BOUNDARIES:
      os << indent << "SamplingPattern: SAMPLE_LINE_AT_CELL_BOUNDARIES" << endl;
      break;
    case SAMPLE_LINE_AT_SEGMENT_CENTERS:
      os << indent << "SamplingPattern: SAMPLE_LINE_AT_SEGMENT_CENTERS" << endl;
      break;
    case SAMPLE_LINE_UNIFORMLY:
      os << indent << "SamplingPattern: SAMPLE_LINE_UNIFORMLY" << endl;
      break;
    default:
      os << indent << "SamplingPattern: UNDEFINED" << endl;
      break;
  }
  os << indent << "LineResolution: " << this->LineResolution << endl;
  os << indent << "AggregateAsPolyData: " << this->AggregateAsPolyData << endl;
  os << indent << "PassPartialArrays: " << this->PassPartialArrays << endl;
  os << indent << "PassCellArrays: " << this->PassCellArrays << endl;
  os << indent << "PassPointArrays: " << this->PassPointArrays << endl;
  os << indent << "PassFieldArrays: " << this->PassFieldArrays << endl;
  os << indent << "ComputeTolerance: " << this->ComputeTolerance << endl;
  os << indent << "Tolerance: " << this->Tolerance << endl;
}
