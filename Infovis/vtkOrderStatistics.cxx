/*=========================================================================

Program:   Visualization Toolkit
Module:    vtkOrderStatistics.cxx

Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
All rights reserved.
See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

This software is distributed WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
/*-------------------------------------------------------------------------
  Copyright 2010 Sandia Corporation.
  Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
  the U.S. Government retains certain rights in this software.
  -------------------------------------------------------------------------*/

#include "vtkToolkits.h"

#include "vtkOrderStatistics.h"
#include "vtkStatisticsAlgorithmPrivate.h"

#include "vtkDoubleArray.h"
#include "vtkIdTypeArray.h"
#include "vtkInformation.h"
#include "vtkIntArray.h"
#include "vtkObjectFactory.h"
#include "vtkMath.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkStringArray.h"
#include "vtkTable.h"
#include "vtkVariantArray.h"

#include <vtksys/stl/vector>
#include <vtksys/stl/map>
#include <vtksys/stl/set>

typedef vtksys_stl::map<vtkStdString,double> CDF;

vtkStandardNewMacro(vtkOrderStatistics);

// ----------------------------------------------------------------------
vtkOrderStatistics::vtkOrderStatistics()
{
  this->QuantileDefinition = vtkOrderStatistics::InverseCDFAveragedSteps;
  this->NumberOfIntervals = 4; // By default, calculate 5-points statistics

  this->AssessNames->SetNumberOfValues( 1 );
  this->AssessNames->SetValue( 0, "Quantile" );
}

// ----------------------------------------------------------------------
vtkOrderStatistics::~vtkOrderStatistics()
{
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::PrintSelf( ostream &os, vtkIndent indent )
{
  this->Superclass::PrintSelf( os, indent );
  os << indent << "NumberOfIntervals: " << this->NumberOfIntervals << endl;
  os << indent << "QuantileDefinition: " << this->QuantileDefinition << endl;
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::SetQuantileDefinition( int qd )
{
  switch ( qd )
    {
    case vtkOrderStatistics::InverseCDF:
      break;
    case vtkOrderStatistics::InverseCDFAveragedSteps:
      break;
    default:
      vtkWarningMacro( "Incorrect type of quantile definition: "
                       <<qd
                       <<". Ignoring it." );
      return;
    }

  this->QuantileDefinition =  static_cast<vtkOrderStatistics::QuantileDefinitionType>( qd );
  this->Modified();

  return;
}

// ----------------------------------------------------------------------
bool vtkOrderStatistics::SetParameter( const char* parameter,
                                       int vtkNotUsed( index ),
                                       vtkVariant value )
{
  if ( ! strcmp( parameter, "NumberOfIntervals" ) )
    {
    this->SetNumberOfIntervals( value.ToInt() );

    return true;
    }

  if ( ! strcmp( parameter, "QuantileDefinition" ) )
    {
    this->SetQuantileDefinition( value.ToInt() );

    return true;
    }

  return false;
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::Learn( vtkTable* inData,
                                vtkTable* vtkNotUsed( inParameters ),
                                vtkMultiBlockDataSet* outMeta )
{
  if ( ! inData )
    {
    return;
    }

  if ( ! outMeta )
    {
    return;
    }

  // Loop over requests
  vtkIdType nRow = inData->GetNumberOfRows();
  for ( vtksys_stl::set<vtksys_stl::set<vtkStdString> >::iterator rit = this->Internals->Requests.begin();
        rit != this->Internals->Requests.end(); ++ rit )
    {
    // Each request contains only one column of interest (if there are others, they are ignored)
    vtksys_stl::set<vtkStdString>::const_iterator it = rit->begin();
    vtkStdString col = *it;
    if ( ! inData->GetColumnByName( col ) )
      {
      vtkWarningMacro( "InData table does not have a column "
                       << col.c_str()
                       << ". Ignoring it." );
      continue;
      }

    // Get hold of data for this variable
    vtkAbstractArray* vals = inData->GetColumnByName( col );

    // Create histogram table for this variable
    vtkTable* histogramTab = vtkTable::New();

    // Row to be used to insert into histogram table
    vtkVariantArray* row = vtkVariantArray::New();
    row->SetNumberOfValues( 2 );

    // Switch depending on data type
    if ( vals->IsA("vtkDataArray") )
      {
      vtkDoubleArray* doubleCol = vtkDoubleArray::New();
      doubleCol->SetName( "Value" );
      histogramTab->AddColumn( doubleCol );
      doubleCol->Delete();

      // Value of cardinality row is NaN
      double noVal = vtkMath::Nan();
      row->SetValue( 0, noVal );
      }
    else if ( vals->IsA("vtkStringArray") )
      {
      vtkStringArray* stringCol = vtkStringArray::New();
      stringCol->SetName( "Value" );
      histogramTab->AddColumn( stringCol );
      stringCol->Delete();

      // Value of cardinality row is the empty string
      vtkStdString noVal = vtkStdString( "" );
      row->SetValue( 0, noVal );
      }
    else if ( vals->IsA("vtkVariantArray") )
      {
      vtkVariantArray* variantCol = vtkVariantArray::New();
      variantCol->SetName( "Value" );
      histogramTab->AddColumn( variantCol );
      variantCol->Delete();

      // Value of cardinality row is the empty variant
      vtkVariant noVal = vtkVariant( "" );
      row->SetValue( 0, noVal );
      }
    else
      {
      vtkWarningMacro( "Unsupported data type for column "
                       << col.c_str()
                       << ". Ignoring it." );

      continue;
      }

    vtkIdTypeArray* idTypeCol = vtkIdTypeArray::New();
    idTypeCol->SetName( "Cardinality" );
    histogramTab->AddColumn( idTypeCol );
    idTypeCol->Delete();

    // Insert first row which will always contain the data set cardinality
    // NB: The cardinality is calculated in derive mode ONLY, and is set to an invalid value of -1 in
    // learn mode to make it clear that it is not a correct value. This is an issue of database
    // normalization: including the cardinality to the other counts can lead to inconsistency, in particular
    // when the input meta table is calculated by something else than the learn mode (e.g., is specified
    // by the user).
    row->SetValue( 1, -1 );
    histogramTab->InsertNextRow( row );

    // Switch depending on data type
    if ( vals->IsA("vtkDataArray") )
      {
      // Downcast column to double array for efficient data access
      vtkDataArray* darr = vtkDataArray::SafeDownCast( vals );

      // Calculate histogram
      vtksys_stl::map<double,vtkIdType> histogram;
      for ( vtkIdType r = 0; r < nRow; ++ r )
        {
        ++ histogram[darr->GetTuple1( r )];
        }

      // Store histogram
      for ( vtksys_stl::map<double,vtkIdType>::iterator mit = histogram.begin();
            mit != histogram.end(); ++ mit  )
        {
        row->SetValue( 0, mit->first );
        row->SetValue( 1, mit->second );
        histogramTab->InsertNextRow( row );
        }
      }
    else if ( vals->IsA("vtkStringArray") )
      {
      // Downcast column to string array for efficient data access
      vtkStringArray* sarr = vtkStringArray::SafeDownCast( vals );

      // Calculate histogram
      vtksys_stl::map<vtkStdString,vtkIdType> histogram;
      for ( vtkIdType r = 0; r < nRow; ++ r )
        {
        ++ histogram[sarr->GetValue( r )];
        }

      // Store histogram
      for ( vtksys_stl::map<vtkStdString,vtkIdType>::iterator mit = histogram.begin();
            mit != histogram.end(); ++ mit  )
        {
        row->SetValue( 0, mit->first );
        row->SetValue( 1, mit->second );
        histogramTab->InsertNextRow( row );
        }
      }
    else if ( vals->IsA("vtkVariantArray") )
      {
      // Downcast column to string array for efficient data access
      vtkVariantArray* varr = vtkVariantArray::SafeDownCast( vals );

      // Calculate histogram
      vtksys_stl::map<vtkVariant,vtkIdType> histogram;
      for ( vtkIdType r = 0; r < nRow; ++ r )
        {
        ++ histogram[varr->GetVariantValue( r )];
        }

      // Store histogram
      for ( vtksys_stl::map<vtkVariant,vtkIdType>::iterator mit = histogram.begin();
            mit != histogram.end(); ++ mit  )
        {
        row->SetValue( 0, mit->first );
        row->SetValue( 1, mit->second );
        histogramTab->InsertNextRow( row );
        }
      }
    else
      {
      vtkWarningMacro( "Unsupported data type for column "
                       << col.c_str()
                       << ". Ignoring it." );

      continue;
      }

    // Resize output meta so histogram table can be appended
    unsigned int nBlocks = outMeta->GetNumberOfBlocks();
    outMeta->SetNumberOfBlocks( nBlocks + 1 );
    outMeta->GetMetaData( static_cast<unsigned>( nBlocks ) )->Set( vtkCompositeDataSet::NAME(), col );
    outMeta->SetBlock( nBlocks, histogramTab );

    // Clean up
    histogramTab->Delete();
    row->Delete();
    } // rit

  return;
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::Derive( vtkMultiBlockDataSet* inMeta )
{
  if ( ! inMeta || inMeta->GetNumberOfBlocks() < 1 )
    {
    return;
    }

  // Create quantiles table
  vtkTable* quantileTab = vtkTable::New();

  vtkStringArray* stringCol = vtkStringArray::New();
  stringCol->SetName( "Quantile" );
  quantileTab->AddColumn( stringCol );
  stringCol->Delete();

  double dq = 1. / static_cast<double>( this->NumberOfIntervals );
  for ( int i = 0; i <= this->NumberOfIntervals; ++ i )
    {

    // Handle special case of quartiles and median for convenience
    div_t q = div( i << 2, this->NumberOfIntervals );
    if ( q.rem )
      {
      // General case
      stringCol->InsertNextValue( vtkStdString( vtkVariant( i * dq ).ToString() + "-quantile" ).c_str() );
      }
    else
      {
      // Case where q is a multiple of 4
      switch ( q.quot )
        {
        case 0:
          stringCol->InsertNextValue( "Minimum" );
          break;
        case 1:
          stringCol->InsertNextValue( "First Quartile" );
          break;
        case 2:
          stringCol->InsertNextValue( "Median" );
          break;
        case 3:
          stringCol->InsertNextValue( "Third Quartile" );
          break;
        case 4:
          stringCol->InsertNextValue( "Maximum" );
          break;
        default:
          stringCol->InsertNextValue( vtkStdString( vtkVariant( i * dq ).ToString() + "-quantile" ).c_str() );
          break;
        }
      }
    }

  // Iterate over primary tables
  unsigned int nBlocks = inMeta->GetNumberOfBlocks();
  for ( unsigned int b = 0; b < nBlocks; ++ b )
    {
    vtkTable* histogramTab = vtkTable::SafeDownCast( inMeta->GetBlock( b ) );
    if ( ! histogramTab  )
      {
      continue;
      }

    // Downcast columns to typed arrays for efficient data access
    vtkStringArray* vals = vtkStringArray::SafeDownCast( histogramTab->GetColumnByName( "Value" ) );
    vtkIdTypeArray* card = vtkIdTypeArray::SafeDownCast( histogramTab->GetColumnByName( "Cardinality" ) );

    // Calculate variable cardinality
    vtkIdType c;
    vtkIdType n = 0;
    vtkIdType nRowHist = histogramTab->GetNumberOfRows();
    for ( int r = 1; r < nRowHist; ++ r ) // Skip first row where data set cardinality will be stored
      {
      //x = vals->GetValue( r );

      // Update cardinality
      c = card->GetValue( r );
      n += c;
      }

    // Store cardinality
    histogramTab->SetValueByName( 0, "Cardinality", n );

    // Find or create column of probability mass function of histogram table
    vtkStdString probaName( "P" );
    vtkDoubleArray* probaCol;
    vtkAbstractArray* abstrCol = histogramTab->GetColumnByName( probaName );
    if ( ! abstrCol )
      {
      probaCol = vtkDoubleArray::New();
      probaCol->SetName( probaName );
      probaCol->SetNumberOfTuples( nRowHist );
      histogramTab->AddColumn( probaCol );
      probaCol->Delete();
      }
    else
      {
      probaCol = vtkDoubleArray::SafeDownCast( abstrCol );
      }

    // Store invalid probability for cardinality row
    histogramTab->SetValueByName( 0, "P", -1. );

    // Finally calculate and store probabilities
    double inv_n = 1. / n;
    double p;
    for ( int r = 1; r < nRowHist; ++ r ) // Skip first row which contains data set cardinality
      {
      c = card->GetValue( r );
      p = inv_n * c;

      probaCol->SetValue( r, p );
      }

    // Now calculate and store quantile thresholds
    vtksys_stl::vector<double> quantileThresholds;
    double dh = n / static_cast<double>( this->NumberOfIntervals );
    for ( int j = 0; j < this->NumberOfIntervals; ++ j )
      {
      quantileThresholds.push_back( j * dh );
      }

//    quantileTab->InsertNextRow( rowQuant );

    histogramTab->Dump();

    } // for ( unsigned int b = 0; b < nBlocks; ++ b )

  quantileTab->Dump();

//   // Finally calculate quantiles and store them iterating over variables
//   vtkStdString col;
//   for ( vtksys_stl::map<vtkIdType,vtksys_stl::map<vtkStdString,vtkIdType> >::iterator mit = marginalCounts.begin();
//         mit != marginalCounts.end(); ++ mit  )
//     {
//     // Get variable and name and set corresponding row value
//     col = vars->GetValue( mit->first );
//     rowQuant->SetValue( 0, col );

//     // Also set cardinality which is known
//     rowQuant->SetValue( 1, n );

//     // Then calculate quantiles
//     vtkIdType sum = 0;
//     int j = 2;
//     vtksys_stl::vector<double>::iterator qit = quantileThresholds.begin();
//     for ( vtksys_stl::map<vtkStdString,vtkIdType>::iterator nit = mit->second.begin();
//           nit != mit->second.end(); ++ nit  )
//       {
//       for ( sum += nit->second; qit != quantileThresholds.end() && sum >= *qit; ++ qit )
//         {
//         // No mid-point interpolation for non-numeric types
//         rowQuant->SetValue( j ++, nit->first );
//         } // qit
//       } // nit

//     // Finally store quantiles for this variable after a last sanity check
//     if ( j != this->NumberOfIntervals + 2 )
//       {
//       vtkErrorMacro( "Inconsistent quantile table: calculated "
//                      << j - 1
//                      << " quantiles != "
//                      << this->NumberOfIntervals + 1
//                      <<". Cannot derive model." );
//       return;
//       }

//     rowQuant->SetValue( j, mit->second.rbegin()->first );
//     } // mit

  // Resize output meta so quantile table can be appended
  nBlocks = inMeta->GetNumberOfBlocks();
  inMeta->SetNumberOfBlocks( nBlocks + 1 );
  inMeta->GetMetaData( static_cast<unsigned>( nBlocks ) )->Set( vtkCompositeDataSet::NAME(), "Quantiles" );
  inMeta->SetBlock( nBlocks, quantileTab );

  // Clean up
  quantileTab->Delete();
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::Test( vtkTable* inData,
                               vtkMultiBlockDataSet* inMeta,
                               vtkTable* outMeta )
{
  if ( ! inMeta )
    {
    return;
    }

  vtkTable* quantileTab = vtkTable::SafeDownCast( inMeta->GetBlock( 2 ) );
  if ( ! quantileTab )
    {
    return;
    }

  if ( ! outMeta )
    {
    return;
    }

  // Prepare columns for the test:
  // 0: variable name
  // 1: Maximum vertical distance between CDFs
  // 2: Kolmogorov-Smirnov test statistic (the above times the square root of the cardinality)
  // NB: These are not added to the output table yet, for they will be filled individually first
  //     in order that R be invoked only once.
  vtkStringArray* nameCol = vtkStringArray::New();
  nameCol->SetName( "Variable" );

  vtkDoubleArray* distCol = vtkDoubleArray::New();
  distCol->SetName( "Maximum Distance" );

  vtkDoubleArray* statCol = vtkDoubleArray::New();
  statCol->SetName( "Kolomogorov-Smirnov" );

  // Downcast columns to string arrays for efficient data access
  vtkStringArray* vars = vtkStringArray::SafeDownCast( quantileTab->GetColumnByName( "Variable" ) );

  // Prepare storage for quantiles and model CDFs
  vtkIdType nQuant = quantileTab->GetNumberOfColumns() - 2;
  vtkStdString* quantiles = new vtkStdString[nQuant];

  // Loop over requests
  vtkIdType nRowQuant = quantileTab->GetNumberOfRows();
  vtkIdType nRowData = inData->GetNumberOfRows();
  double inv_nq =  1. / nQuant;
  double inv_card = 1. / nRowData;
  double sqrt_card = sqrt( static_cast<double>( nRowData ) );
  for ( vtksys_stl::set<vtksys_stl::set<vtkStdString> >::const_iterator rit = this->Internals->Requests.begin();
        rit != this->Internals->Requests.end(); ++ rit )
    {
    // Each request contains only one column of interest (if there are others, they are ignored)
    vtksys_stl::set<vtkStdString>::const_iterator it = rit->begin();
    vtkStdString varName = *it;
    if ( ! inData->GetColumnByName( varName ) )
      {
      vtkWarningMacro( "InData table does not have a column "
                       << varName.c_str()
                       << ". Ignoring it." );
      continue;
      }

    // Find the model row that corresponds to the variable of the request
    vtkIdType r = 0;
    while ( r < nRowQuant && vars->GetValue( r ) != varName )
      {
      ++ r;
      }
    if ( r >= nRowQuant )
      {
      vtkWarningMacro( "Incomplete input: model does not have a row "
                       << varName.c_str()
                       <<". Cannot test." );
      continue;
      }

    // First iterate over all observations to calculate empirical PDF
    CDF cdfEmpirical;
    for ( vtkIdType j = 0; j < nRowData; ++ j )
      {
      // Read observation and update PDF
      cdfEmpirical
        [inData->GetValueByName( j, varName ).ToString()] += inv_card;
      }

    // Now integrate to obtain empirical CDF
    double sum = 0.;
    for ( CDF::iterator cit = cdfEmpirical.begin(); cit != cdfEmpirical.end(); ++ cit )
      {
      sum += cit->second;
      cit->second = sum;
      }

    // Sanity check: verify that empirical CDF = 1
    if ( fabs( sum - 1. ) > 1.e-6 )
      {
      vtkWarningMacro( "Incorrect empirical CDF for variable:"
                       << varName.c_str()
                       << ". Ignoring it." );

      continue;
      }

    // Retrieve quantiles to calculate model CDF and insert value into empirical CDF
    for ( vtkIdType i = 0; i < nQuant; ++ i )
      {
      // Read quantile and update CDF
      quantiles[i] = quantileTab->GetValue( r, i + 2 ).ToString();

      // Update empirical CDF if new value found (with unknown ECDF)
      vtksys_stl::pair<CDF::iterator,bool> result
        = cdfEmpirical.insert( vtksys_stl::pair<vtkStdString,double>( quantiles[i], -1 ) );
      if ( result.second == true )
        {
        CDF::iterator eit = result.first;
        // Check if new value has no predecessor, in which case CDF = 0
        if ( eit ==  cdfEmpirical.begin() )
          {
          result.first->second = 0.;
          }
        else
          {
          -- eit;
          result.first->second = eit->second;
          }
        }
      }

    // Iterate over all CDF jump values
    int currentQ = 0;
    double mcdf = 0.;
    double Dmn = 0.;
    for ( CDF::iterator cit = cdfEmpirical.begin(); cit != cdfEmpirical.end(); ++ cit )
      {
      // If observation is smaller than minimum then there is nothing to do
      if ( cit->first >= quantiles[0] )
        {
        while ( currentQ < nQuant && cit->first >= quantiles[currentQ] )
          {
          ++ currentQ;
          }

        // Calculate model CDF at observation
        mcdf = currentQ * inv_nq;
        }

      // Calculate vertical distance between CDFs and update maximum if needed
      double d = fabs( cit->second - mcdf );
      if ( d > Dmn )
        {
        Dmn =  d;
        }
      }

    // Insert variable name and calculated Kolmogorov-Smirnov statistic
    // NB: R will be invoked only once at the end for efficiency
    nameCol->InsertNextValue( varName );
    distCol->InsertNextTuple1( Dmn );
    statCol->InsertNextTuple1( sqrt_card * Dmn );
    } // rit

  // Now, add the already prepared columns to the output table
  outMeta->AddColumn( nameCol );
  outMeta->AddColumn( distCol );
  outMeta->AddColumn( statCol );

  // Clean up
  delete [] quantiles;
  nameCol->Delete();
  distCol->Delete();
  statCol->Delete();
}

// ----------------------------------------------------------------------
class NumericColumnQuantizationFunctor : public vtkStatisticsAlgorithm::AssessFunctor
{
public:
  vtkDataArray* Data;
  double* Quantiles;
  vtkIdType NumberOfValues;

  NumericColumnQuantizationFunctor( vtkAbstractArray* vals,
                                    vtkVariantArray* quantiles )
  {
    this->Data = vtkDataArray::SafeDownCast( vals );
    this->NumberOfValues = quantiles->GetNumberOfValues() - 2;
    this->Quantiles = new double[quantiles->GetNumberOfValues()];
    for ( int q = 0; q < this->NumberOfValues; ++ q )
      {
      // Skip value #0 which is the variable name and #1 which is the cardinality
      this->Quantiles[q] = quantiles->GetValue( q + 2 ).ToDouble();
      }
  }
  virtual ~NumericColumnQuantizationFunctor()
  {
    delete [] this->Quantiles;
  }
  virtual void operator() ( vtkVariantArray* result,
                            vtkIdType id )
  {
    double x = this->Data->GetTuple1( id );

    if ( x < this->Quantiles[0] )
      {
      // x is smaller than lower bound
      result->SetNumberOfValues( 1 );
      result->SetValue( 0, 0 );

      return;
      }

    vtkIdType q = 1;
    while ( q < this->NumberOfValues && x > this->Quantiles[q] )
      {
      ++ q;
      }

    result->SetNumberOfValues( 1 );
    result->SetValue( 0, q );
  }
};

// ----------------------------------------------------------------------
class StringColumnToDoubleQuantizationFunctor : public vtkStatisticsAlgorithm::AssessFunctor
{
public:
  vtkAbstractArray* Data;
  double* Quantiles;
  vtkIdType NumberOfValues;

  StringColumnToDoubleQuantizationFunctor( vtkAbstractArray* vals,
                                           vtkVariantArray* quantiles )
  {
    this->Data = vals;
    this->NumberOfValues = quantiles->GetNumberOfValues() - 2;
    this->Quantiles = new double[quantiles->GetNumberOfValues()];
    for ( int q = 0; q < this->NumberOfValues; ++ q )
      {
      // Skip value #0 which is the variable name and #1 which is the cardinality
      this->Quantiles[q] = quantiles->GetValue( q + 2 ).ToDouble();
      }
  }
  virtual ~StringColumnToDoubleQuantizationFunctor()
  {
    delete [] this->Quantiles;
  }
  virtual void operator() ( vtkVariantArray* result,
                            vtkIdType id )
  {
    double x = this->Data->GetVariantValue( id ).ToDouble();

    if ( x < this->Quantiles[0] )
      {
      // x is smaller than lower bound
      result->SetNumberOfValues( 1 );
      result->SetValue( 0, 0 );

      return;
      }

    vtkIdType q = 1;
    while ( q < this->NumberOfValues && x > this->Quantiles[q] )
      {
      ++ q;
      }

    result->SetNumberOfValues( 1 );
    result->SetValue( 0, q );
  }
};

// ----------------------------------------------------------------------
class StringColumnQuantizationFunctor : public vtkStatisticsAlgorithm::AssessFunctor
{
public:
  vtkAbstractArray* Data;
  vtkStdString* Quantiles;
  vtkIdType NumberOfValues;

  StringColumnQuantizationFunctor( vtkAbstractArray* vals,
                                   vtkVariantArray* quantiles )
  {
    this->Data = vals;
    this->NumberOfValues = quantiles->GetNumberOfValues() - 2;
    this->Quantiles = new vtkStdString[quantiles->GetNumberOfValues()];
    for ( int q = 0; q < this->NumberOfValues; ++ q )
      {
      // Skip value #0 which is the variable name and #1 which is the cardinality
      this->Quantiles[q] = quantiles->GetValue( q + 2 ).ToString();
      }
  }
  virtual ~StringColumnQuantizationFunctor()
  {
    delete [] this->Quantiles;
  }
  virtual void operator() ( vtkVariantArray* result,
                            vtkIdType id )
  {
    vtkStdString x = this->Data->GetVariantValue( id ).ToString();

    if ( x < this->Quantiles[0] )
      {
      // x is smaller than lower bound
      result->SetNumberOfValues( 1 );
      result->SetValue( 0, 0 );

      return;
      }

    vtkIdType q = 1;
    while ( q < this->NumberOfValues && x > this->Quantiles[q] )
      {
      ++ q;
      }

    result->SetNumberOfValues( 1 );
    result->SetValue( 0, q );
  }
};

// ----------------------------------------------------------------------
void vtkOrderStatistics::SelectAssessFunctor( vtkTable* outData,
                                              vtkDataObject* inMetaDO,
                                              vtkStringArray* rowNames,
                                              AssessFunctor*& dfunc )
{
  vtkMultiBlockDataSet* inMeta = vtkMultiBlockDataSet::SafeDownCast( inMetaDO );
  if ( ! inMeta
       || inMeta->GetNumberOfBlocks() < 1 )
    {
    return;
    }

  vtkTable* quantileTab = vtkTable::SafeDownCast( inMeta->GetBlock( 2 ) );
  if ( ! quantileTab )
    {
    return;
    }

  vtkStdString varName = rowNames->GetValue( 0 );

  // Downcast meta columns to string arrays for efficient data access
  vtkStringArray* vars = vtkStringArray::SafeDownCast( quantileTab->GetColumnByName( "Variable" ) );
  if ( ! vars )
    {
    dfunc = 0;
    return;
    }

  // Loop over parameters table until the requested variable is found
  vtkIdType nRowP = quantileTab->GetNumberOfRows();
  for ( int r = 0; r < nRowP; ++ r )
    {
    if ( vars->GetValue( r ) == varName )
      {
      // Grab the data for the requested variable
      vtkAbstractArray* vals = outData->GetColumnByName( varName );
      if ( ! vals )
        {
        dfunc = 0;
        return;
        }

      // Select assess functor
      else
        {
        dfunc = new StringColumnQuantizationFunctor( vals, quantileTab->GetRow( r ) );
        }
      return;
      }
    }

  // The variable of interest was not found in the parameter table
  dfunc = 0;
}
