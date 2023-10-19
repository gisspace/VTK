/*=========================================================================

    Program:   Visualization Toolkit
    Module:    vtkAnariFollowerNode.h

    Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
    All rights reserved.
    See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

        This software is distributed WITHOUT ANY WARRANTY; without even
        the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
        PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
/**
 * @class   vtkAnariFollowerNode
 * @brief   links vtkFollower to ANARI
 *
 * Translates vtkFollower state into ANARI state
 *
 * @par Thanks:
 * Kevin Griffin kgriffin@nvidia.com for creating and contributing the class
 * and NVIDIA for supporting this work.
 */

#ifndef vtkAnariFollowerNode_h
#define vtkAnariFollowerNode_h

#include "vtkAnariActorNode.h"

VTK_ABI_NAMESPACE_BEGIN

class VTKRENDERINGANARI_EXPORT vtkAnariFollowerNode : public vtkAnariActorNode
{
public:
  static vtkAnariFollowerNode* New();
  vtkTypeMacro(vtkAnariFollowerNode, vtkAnariActorNode);
  void PrintSelf(ostream& os, vtkIndent indent) override;

  /**
   * Overridden to take into account this renderables time, including
   * its associated camera
   */
  vtkMTimeType GetMTime() override;

protected:
  vtkAnariFollowerNode() = default;
  ~vtkAnariFollowerNode() = default;

private:
  vtkAnariFollowerNode(const vtkAnariFollowerNode&) = delete;
  void operator=(const vtkAnariFollowerNode&) = delete;
};

VTK_ABI_NAMESPACE_END
#endif
