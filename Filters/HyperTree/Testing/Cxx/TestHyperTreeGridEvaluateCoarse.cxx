// SPDX-FileCopyrightText: Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkCellData.h"
#include "vtkDataArray.h"
#include "vtkHyperTreeGrid.h"
#include "vtkHyperTreeGridEvaluateCoarse.h"
#include "vtkHyperTreeGridNonOrientedGeometryCursor.h"
#include "vtkMathUtilities.h"
#include "vtkNew.h"
#include "vtkRandomHyperTreeGridSource.h"

#include <cmath>
#include <numeric>
#include <vector>

namespace
{
constexpr int MAX_DEPTH = 5;
constexpr int CHILD_FACTOR = 8;

bool IsInRange(double sum, int level)
{
  int maxSum = MAX_DEPTH * std::pow(CHILD_FACTOR, MAX_DEPTH - level);
  if (sum > maxSum || sum < 0)
  {
    vtkWarningWithObjectMacro(nullptr,
      "Sum out of range : got " << sum << " for level " << level << " but expected less than "
                                << maxSum);
    return false;
  }
  return true;
}

bool CheckTree(
  vtkHyperTreeGridNonOrientedGeometryCursor* cursorOut, vtkDataArray* depthOut, int level)
{
  vtkIdType currentId = cursorOut->GetGlobalNodeIndex();

  if (cursorOut->IsLeaf() || cursorOut->IsMasked())
  {
    return true;
  }

  // Recurse over children
  bool result = true;
  for (int child = 0; child < cursorOut->GetNumberOfChildren(); ++child)
  {
    cursorOut->ToChild(child);
    result &= ::CheckTree(cursorOut, depthOut, level + 1);
    cursorOut->ToParent();
  }

  return result && IsInRange(depthOut->GetTuple1(currentId), level);
}
}

int TestHyperTreeGridEvaluateCoarse(int, char*[])
{
  vtkNew<vtkRandomHyperTreeGridSource> source;
  source->SetDimensions(5, 5, 5);
  source->SetMaxDepth(MAX_DEPTH);
  // source->SetMaskedFraction(0.2);
  source->SetSeed(3);
  source->SetSplitFraction(1.0);

  vtkNew<vtkHyperTreeGridEvaluateCoarse> evaluate;
  evaluate->SetInputConnection(source->GetOutputPort());
  evaluate->SetOperator(vtkHyperTreeGridEvaluateCoarse::OPERATOR_SUM);
  evaluate->Update();

  vtkHyperTreeGrid* inputHTG = source->GetHyperTreeGridOutput();
  vtkHyperTreeGrid* outputHTG = evaluate->GetHyperTreeGridOutput();

  vtkDataArray* depthOut = vtkDataArray::SafeDownCast(outputHTG->GetCellData()->GetArray("Depth"));

  // Iterate over the output tree, and check the computed output field
  vtkIdType index = 0;
  vtkHyperTreeGrid::vtkHyperTreeGridIterator iteratorOut;
  vtkHyperTreeGrid::vtkHyperTreeGridIterator iteratorIn;
  outputHTG->InitializeTreeIterator(iteratorOut);
  inputHTG->InitializeTreeIterator(iteratorIn);
  vtkNew<vtkHyperTreeGridNonOrientedGeometryCursor> outCursor;
  vtkNew<vtkHyperTreeGridNonOrientedGeometryCursor> inCursor;
  while (iteratorIn.GetNextTree(index))
  {
    inputHTG->InitializeNonOrientedGeometryCursor(outCursor, index);
    outputHTG->InitializeNonOrientedGeometryCursor(inCursor, index);
    if (!::CheckTree(outCursor, depthOut, 0))
    {
      std::cerr << "Node " << index << " failed validation." << std::endl;
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}
