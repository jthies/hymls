// @HEADER
// ************************************************************************
//
//           Galeri: Finite Element and Matrix Generation Package
//                 Copyright (2006) ETHZ/Sandia Corporation
//
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
//
// This library is free software; you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as
// published by the Free Software Foundation; either version 2.1 of the
// License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
// USA
//
// Questions about Galeri? Contact Marzio Sala (marzio.sala _AT_ gmail.com)
//
// ************************************************************************
// @HEADER

#ifndef GALERIEXT_DARCY2D_H
#define GALERIEXT_DARCY2D_H

#include "Galeri_Exception.h"
#include "Galeri_Utils.h"
#include "Epetra_Comm.h"
#include "Epetra_BlockMap.h"
#include "Epetra_CrsMatrix.h"

#include "GaleriExt_Periodic.h"

namespace GaleriExt {
namespace Matrices {

//! generate an F-matrix with A = diag(a) and +b -b in the B part
template<typename int_type>
inline
Epetra_CrsMatrix*
Darcy2D(const Epetra_Map* Map,
        const int nx, const int ny,
        const double a, const double b,
        PERIO_Flag perio=NO_PERIO)
{
  Epetra_CrsMatrix* Matrix = new Epetra_CrsMatrix(Copy, *Map,  4);

  int NumMyElements = Map->NumMyElements();
  int_type* MyGlobalElements = 0;
  Map->MyGlobalElementsPtr(MyGlobalElements);

  int left, right, lower, upper;

  std::vector<double> Values(4);
  std::vector<int_type> Indices(4);

  double c = -b; // c==b => [A B'; B 0]. c==-b => A B'; -B 0]

  int dof = 3;

  if (dof*nx*ny != Map->NumGlobalElements64())
  {
    throw("bad input map for GaleriExt::Darcy2D. Should have 3 dof/node");
  }

  for (int i = 0 ; i < NumMyElements ; i++)
  {
    int NumEntries = 0;

    int_type ibase = std::floor(MyGlobalElements[i]/dof);
    int_type ivar   = MyGlobalElements[i]-ibase*dof;
    // first the regular 7-point stencil
    GetNeighboursCartesian2d(ibase, nx, ny,
                             left, right, lower, upper,
                             perio);

    if (ivar!=2)
    {
      NumEntries=1;
      Values[0]=a;
      Indices[0]=MyGlobalElements[i];
      if (right != -1 && ivar==0)
      {
        Indices[NumEntries] = ibase*dof+2;
        Values[NumEntries] = -b;
        ++NumEntries;
        Indices[NumEntries] = right*dof+2;
        Values[NumEntries] = b;
        ++NumEntries;
      }
      if (upper != -1 && ivar==1)
      {
        Indices[NumEntries] = ibase*dof+2;
        Values[NumEntries] = -b;
        ++NumEntries;
        Indices[NumEntries] = upper*dof+2;
        Values[NumEntries] = b;
        ++NumEntries;
      }
    }
    else // P
    {
      // div-rows
      if (right!=-1)
      {
        Indices[NumEntries]=ibase*dof+0;
        Values[NumEntries]=-c;
        NumEntries++;
      }
      if (upper!=-1)
      {
        Indices[NumEntries]=ibase*dof+1;
        Values[NumEntries]=-c;
        NumEntries++;
      }
      if (left!=-1)
      {
        Indices[NumEntries]=left*dof+0;
        Values[NumEntries]=c;
        NumEntries++;
      }
      if (lower!=-1)
      {
        Indices[NumEntries]=lower*dof+1;
        Values[NumEntries]=c;
        NumEntries++;
      }
    }

#ifdef DEBUGGING_
    std::cerr << i << " " << MyGlobalElements[i] << " " << ibase << " " << ivar << std::endl;
    for (int jj=0;jj<NumEntries;jj++)
    {
      std::cerr << Indices[jj] << " ";
    }
    std::cerr << std::endl;
#endif

    // put the off-diagonal entries
    Matrix->InsertGlobalValues(MyGlobalElements[i], NumEntries,
                               &Values[0], &Indices[0]);

  }
  Matrix->FillComplete();
  Matrix->OptimizeStorage();

  return(Matrix);
}

//! generate an B-grid discretization with A = diag(a) and +b -b in the B part
template<typename int_type>
inline
Epetra_CrsMatrix*
DarcyB2D(const Epetra_Map* Map,
        const int nx, const int ny,
        const double a, const double b,
        PERIO_Flag perio=NO_PERIO)
{
  Epetra_CrsMatrix* Matrix = new Epetra_CrsMatrix(Copy, *Map,  8);

  int NumMyElements = Map->NumMyElements();
  int_type* MyGlobalElements = 0;
  Map->MyGlobalElementsPtr(MyGlobalElements);

  int left, right, lower, upper;
  int top_left, top_right, top_lower, top_upper;
  int bottom_left, bottom_right, bottom_lower, bottom_upper;

  std::vector<double> Values(8);
  std::vector<int_type> Indices(8);

  double c = -b; // c==b => [A B'; B 0]. c==-b => A B'; -B 0]

  int dof = 3;

  if (dof*nx*ny != Map->NumGlobalElements64())
  {
    throw("bad input map for GaleriExt::Darcy2D. Should have 4 dof/node");
  }

  for (int i = 0 ; i < NumMyElements ; i++)
  {
    int NumEntries = 0;

    int_type ibase = std::floor(MyGlobalElements[i]/dof);
    int_type ivar   = MyGlobalElements[i]-ibase*dof;
    // first the regular 7-point stencil
    GetNeighboursCartesian2d(ibase, nx, ny,
                             left, right, lower, upper,
                             perio);
    if (upper != -1)
      GetNeighboursCartesian2d(upper, nx, ny,
                               top_left, top_right, top_lower, top_upper,
                               perio);
    if (lower != -1)
      GetNeighboursCartesian2d(lower, nx, ny,
                               bottom_left, bottom_right, bottom_lower, bottom_upper,
                               perio);

    if (ivar!=2)
    {
      NumEntries=1;
      Values[0]=a;
      Indices[0]=MyGlobalElements[i];

      if (right != -1 && upper != -1 && ivar == 0)
      {
        Indices[NumEntries] = ibase*dof+2;
        Values[NumEntries] = -b;
        ++NumEntries;
        Indices[NumEntries] = right*dof+2;
        Values[NumEntries] = b;
        ++NumEntries;
        Indices[NumEntries] = upper*dof+2;
        Values[NumEntries] = -b;
        ++NumEntries;
        Indices[NumEntries] = top_right*dof+2;
        Values[NumEntries] = b;
        ++NumEntries;
      }
      else if (right != -1 && upper != -1 && ivar == 1)
      {
        Indices[NumEntries] = ibase*dof+2;
        Values[NumEntries] = -b;
        ++NumEntries;
        Indices[NumEntries] = right*dof+2;
        Values[NumEntries] = -b;
        ++NumEntries;
        Indices[NumEntries] = upper*dof+2;
        Values[NumEntries] = b;
        ++NumEntries;
        Indices[NumEntries] = top_right*dof+2;
        Values[NumEntries] = b;
        ++NumEntries;
      }
    }
    else // P
    {
      // div-rows
      if (right != -1 && upper != -1)
      {
        Indices[NumEntries]=ibase*dof+0;
        Values[NumEntries]=-c;
        NumEntries++;
        Indices[NumEntries]=ibase*dof+1;
        Values[NumEntries]=-c;
        NumEntries++;
      }
      if (left != -1 && upper != -1)
      {
        Indices[NumEntries]=left*dof+0;
        Values[NumEntries]=c;
        NumEntries++;
        Indices[NumEntries]=left*dof+1;
        Values[NumEntries]=-c;
        NumEntries++;
      }
      if (lower != -1 && right != -1)
      {
        Indices[NumEntries]=lower*dof+0;
        Values[NumEntries]=-c;
        NumEntries++;
        Indices[NumEntries]=lower*dof+1;
        Values[NumEntries]=c;
        NumEntries++;
      }
      if (lower != -1 && left != -1)
      {
        Indices[NumEntries]=bottom_left*dof+0;
        Values[NumEntries]=c;
        NumEntries++;
        Indices[NumEntries]=bottom_left*dof+1;
        Values[NumEntries]=c;
        NumEntries++;
      }
    }

#ifdef DEBUGGING_
    std::cerr << i << " " << MyGlobalElements[i] << " " << ibase << " " << ivar << std::endl;
    for (int jj=0;jj<NumEntries;jj++)
    {
      std::cerr << Indices[jj] << " ";
    }
    std::cerr << std::endl;
#endif

    // put the off-diagonal entries
    Matrix->InsertGlobalValues(MyGlobalElements[i], NumEntries,
                               &Values[0], &Indices[0]);

  }
  Matrix->FillComplete();
  Matrix->OptimizeStorage();

  return(Matrix);
}

inline
Epetra_CrsMatrix*
Darcy2D(const Epetra_Map* Map,
        const int nx, const int ny,
        const double a, const double b,
        PERIO_Flag perio=NO_PERIO,
        char grid_type='C')
{
#ifndef EPETRA_NO_32BIT_GLOBAL_INDICES
  if(Map->GlobalIndicesInt()) {
    if (grid_type == 'C')
      return Darcy2D<int>(Map, nx, ny, a, b, perio);
    else if (grid_type == 'B')
      return DarcyB2D<int>(Map, nx, ny, a, b, perio);
    else
      throw "GaleriExt::Matrices::Darcy2D: Unknown grid type";
  }
  else
#endif
#ifndef EPETRA_NO_64BIT_GLOBAL_INDICES
  if(Map->GlobalIndicesLongLong()) {
    if (grid_type == 'C')
      return Darcy2D<long long>(Map, nx, ny, a, b, perio);
    else if (grid_type == 'B')
      return DarcyB2D<long long>(Map, nx, ny, a, b, perio);
    else
      throw "GaleriExt::Matrices::Darcy2D: Unknown grid type";
  }
  else
#endif
    throw "GaleriExt::Matrices::Darcy2D: GlobalIndices type unknown";
}

} // namespace Matrices
} // namespace GaleriExt
#endif
