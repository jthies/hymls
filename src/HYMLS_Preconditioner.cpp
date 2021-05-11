//#define BLOCK_IMPLEMENTATION 1

#include "HYMLS_Preconditioner.hpp"

#include "HYMLS_config.h"

#include "HYMLS_Tools.hpp"
#include "HYMLS_MatrixUtils.hpp"
#include "HYMLS_DenseUtils.hpp"
#include "HYMLS_OverlappingPartitioner.hpp"

#include "HYMLS_SchurComplement.hpp"
#include "HYMLS_SchurPreconditioner.hpp"
#include "HYMLS_MatrixBlock.hpp"
#include "HYMLS_CoarseSolver.hpp"

#include "Epetra_Comm.h"
#include "Epetra_SerialComm.h"
#include "Epetra_Map.h"
#include "Epetra_RowMatrix.h"
#include "Epetra_Import.h"
#include "Epetra_SerialDenseMatrix.h"
#include "Epetra_BlockMap.h"
#include "Epetra_MultiVector.h"
#include "Epetra_Vector.h"
#include "Epetra_CrsMatrix.h"
#include "Epetra_FECrsMatrix.h"

#include "EpetraExt_MatrixMatrix.h"

#include "HYMLS_Tester.hpp"
#include "HYMLS_Epetra_Time.h"
#include "HYMLS_HierarchicalMap.hpp"
#include "HYMLS_Macros.hpp"

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"
#include "Teuchos_StandardParameterEntryValidators.hpp"
#include "Teuchos_Utils.hpp"

#include <fstream>

namespace HYMLS {

// constructor
Preconditioner::Preconditioner(Teuchos::RCP<const Epetra_RowMatrix> K,
  Teuchos::RCP<Teuchos::ParameterList> params,
  Teuchos::RCP<Epetra_Vector> testVector, int myLevel,
  Teuchos::RCP<const OverlappingPartitioner> hid)
  : PLA("Preconditioner"),
    comm_(Teuchos::rcp(K->Comm().Clone())), matrix_(K),
    rangeMap_(Teuchos::rcp(new Epetra_Map(K->RowMatrixRowMap()))),
    hid_(hid), myLevel_(myLevel), testVector_(testVector),
    useTranspose_(false), normInf_(-1.0),
    label_("Preconditioner"),
    initialized_(false), computed_(false),
    numInitialize_(0), numCompute_(0), numApplyInverse_(0),
    flopsInitialize_(0.0), flopsCompute_(0.0), flopsApplyInverse_(0.0),
    timeInitialize_(0.0), timeCompute_(0.0), timeApplyInverse_(0.0),
    numThreadsSD_(-1), bgridTransform_(false)
  {
  HYMLS_LPROF3(label_,"Constructor");
  serialComm_=Teuchos::rcp(new Epetra_SerialComm());
//    serialComm_=Teuchos::rcp(new Epetra_MpiComm(MPI_COMM_SELF));
  time_=Teuchos::rcp(new Epetra_Time(K->Comm()));

  isPreconditioner_ = true;

  setParameterList(params);
#ifdef HYMLS_DEBUGGING
  dumpVectors_=true;
#endif
  }


// destructor
Preconditioner::~Preconditioner()
  {
  HYMLS_LPROF3(label_,"Destructor");
  }


// Ifpack_Preconditioner interface


// Sets all parameters for the preconditioner.
int Preconditioner::SetParameters(Teuchos::ParameterList& List)
  {
  HYMLS_LPROF3(label_, "SetParameters");

  Teuchos::RCP<Teuchos::ParameterList> List_ =
    getMyNonconstParamList();

  if (List_ == Teuchos::null)
    {
    setMyParamList(Teuchos::rcp(&List, false));
    }
  else if (List_.get() != &List)
    {
    List_->setParameters(List);
    }

  sdSolverType_ = PL().get("Subdomain Solver Type", "Sparse");
  numThreadsSD_ = PL().get("Subdomain Solver Num Threads", numThreadsSD_);
  bgridTransform_ = PL().get("B-Grid Transform", false);
  maxLevel_ = PL().get("Number of Levels", 1);

  if (schurPrec_!=Teuchos::null)
    {
    CHECK_ZERO(schurPrec_->SetParameters(List));
    }

  return 0;
  }

//
void Preconditioner::setParameterList(const Teuchos::RCP<Teuchos::ParameterList>& list)
  {
  HYMLS_LPROF3(label_,"setParameterList");
  setMyParamList(list);
  SetParameters(*list);

  // this is the place where we check for
  // valid parameters for the preconditioner'
  // and Schur preconditioner
  if (validateParameters_)
    {
    getValidParameters();
    PL().validateParameters(VPL());
    }
  HYMLS_DEBVAR(PL());
  }

//
Teuchos::RCP<const Teuchos::ParameterList>
Preconditioner::getValidParameters() const
  {
  if (validParams_!=Teuchos::null) return validParams_;
  HYMLS_LPROF3(label_,"getValidParameters");

  validParams_ = Teuchos::rcp(new Teuchos::ParameterList());

  Teuchos::RCP<Teuchos::StringToIntegralParameterEntryValidator<int> >
    partValidator = Teuchos::rcp(
      new Teuchos::StringToIntegralParameterEntryValidator<int>(
        Teuchos::tuple<std::string>("Cartesian", "Skew Cartesian"),"Partitioner"));

  VPL().set("Partitioner", "Cartesian",
    "Type of partitioner to be used to define the subdomains",
    partValidator);

  VPL().set("Fix Pressure Level", true,
    "Put a Dirichlet condition on a single P-node on the coarsest grid");

  VPL().set("Eliminate Retained Nodes Together", true,
    "Eliminate split separator groups formed by retaining more nodes per separator together");

  VPL().set("Eliminate Velocities Together", false,
    "Eliminate non-Vsum velocities (u,v,w) that are on the same separaotor together");

  for (int i = 1; i < 5; i++)
    {
    VPL().set("Fix GID " + Teuchos::toString(i), -1,
      "Put a Dirichlet condition for node x in the last Schur \n"
      "complement. This is useful for e.g. fixing the pressure \n"
      "level.");
    }

  int sepx=4;
  std::string doc = "Defines the subdomain size for Cartesian partitioning";

  VPL().set("Visualize Solver", false, "write matlab files to visualize the partitioning");

  VPL().set("Separator Length", sepx,doc+" (square subdomains)");
  VPL().set("Separator Length (x)", sepx, doc);
  VPL().set("Separator Length (y)", sepx, doc);
  VPL().set("Separator Length (z)", 1, doc);

  std::string doc2 = "Defines the coarsening factor of the subdomains size at each level";

  VPL().set("Coarsening Factor", sepx, doc2);
  VPL().set("Coarsening Factor (x)", sepx, doc2);
  VPL().set("Coarsening Factor (y)", sepx, doc2);
  VPL().set("Coarsening Factor (z)", 1, doc2);

  Teuchos::RCP<Teuchos::StringToIntegralParameterEntryValidator<int> >
    varValidator = Teuchos::rcp(new Teuchos::StringToIntegralParameterEntryValidator<int>(
        Teuchos::tuple<std::string>(
          "Laplace", "Pressure", "Velocity", "Velocity U", "Velocity V", "Velocity W"),
        "Variable Type"));
  for (int i = 0; i < 6; i++)
    {
    Teuchos::ParameterList& varList = VPL().sublist("Variable "+Teuchos::toString(i),
      false, "For each of the dofs in the problem, a list like this instructs the "
      "BasePartitioner object how to treat the variable."
      "For some pre-defined problems which you can select by setting the "
      "'Problem'->'Equations' parameter, the lists are generated by the "
      "Preconditioner object and you don't have to worry about them.");
    varList.set("Variable Type", "Laplace",
      "describes how the variable should be treated by the partitioner",
      varValidator);
    }

  VPL().set("Subdivide Separators", false,
    "this was implemented for the rotated B-grid and is not intended for any other "
    "problems right now");
  VPL().set("Subdivide based on variable", -1,
    "decide to which variables on a separator to apply the OT based on couplings to \n"
    " this variable (typically the pressure). Not intended for general use");

  VPL().set("Number of Levels", 1,
    "number of levels - on level k a direct solver is used. k = 0 is a direct method\n"
    " for the complete problem, k = 1 is the standard 'two-level' scheme, k > 1 means \n"
    "recursive application of the approximation technique");

  Teuchos::RCP<Teuchos::StringToIntegralParameterEntryValidator<int> >
    solverTypeValidator = Teuchos::rcp(
      new Teuchos::StringToIntegralParameterEntryValidator<int>(
        Teuchos::tuple<std::string>("Sparse", "Dense", "Amesos"),"Subdomain Solver Type"));

  VPL().set("Subdomain Solver Type", "Sparse",
    "Sparse or dense subdomain solver?", solverTypeValidator);

  VPL().set("Dense Solvers on Level", 99,
    "Switch to dense subdomain solver on levels larger than this value");

  VPL().set("Subdomain Solver Num Threads", -1,
    "Set number of OMP/MKL threads before calling subdomain solver, -1: don't "
    "(default)");

  // this typically doesn't need parameters, it's just lapack on small dense
  // matrices.
  VPL().sublist("Dense Solver", false,
    "settings for serial dense solves inside the preconditioner").disableRecursiveValidation();

  VPL().sublist("Sparse Solver", false,
    "settings for serial sparse solvers (passed to Ifpack)").disableRecursiveValidation();

  VPL().sublist("Coarse Solver", false,
    "settings for serial or parallel solver used on the last level "
    " (passed to Ifpack_Amesos)").disableRecursiveValidation();

  Teuchos::RCP<Teuchos::StringToIntegralParameterEntryValidator<int> >
    variantValidator = Teuchos::rcp(
      new Teuchos::StringToIntegralParameterEntryValidator<int>(
        Teuchos::tuple<std::string>
        ("Block Diagonal","Lower Triangular","Domain Decomposition","Do Nothing"),
        "Preconditioner Variant"));

  VPL().set("Preconditioner Variant", "Block Diagonal",
    "Type of approximation used for the non-Vsums:\n"
    "'Block Diagonal' - one dense block per separator group (cf. SIMAX paper)\n"
    "'Block Lower Triangular' - not implemented yet\n"
    "'Domain Decomposition' - one sparse block per processor",
    variantValidator);

  VPL().set("Apply Dropping", true, "Whether dropping is applied in the Schur complement");

  VPL().set("Apply Orthogonal Transformation", true, "Whether or not to apply the orthogonal transformation before dropping. In practice this should only be set to false in case \"Apply Dropping\" is set to false, in which case that is the default.");

  VPL().set("B-Grid Transform", false, "Apply a transformation to turn a B-grid type matrix into an F-matrix");

  std::string retainExtensions[4] = {"", " (x)", " (y)", " (z)"};
  for (std::string const &extension : retainExtensions)
    {
    VPL().set("Retain Nodes" + extension, 1, "Amount of nodes that are retained per separator");

    // FIXME: Can this be done without a for loop?
    for (int i = 0; i < 10; i++)
      VPL().set("Retain Nodes at Level " + Teuchos::toString(i) + extension, 1,
        "Amount of nodes that are retained per separator at level "
        + Teuchos::toString(i));
    }

  return validParams_;
  }

// Computes all it is necessary to initialize the preconditioner.
int Preconditioner::Initialize()
  {
  HYMLS_LPROF(label_,"Initialize");
  time_->ResetStartTime();
  if (hid_==Teuchos::null)
    {
    HYMLS_DEBVAR(*getMyNonconstParamList());
    // this is the partitioning step:
    // - partition domain into small subdomains
    // - find separators
    // - group them according to the needs of our algorithm
    hid_ = Teuchos::rcp(new
      HYMLS::OverlappingPartitioner(rangeMap_,
        getMyNonconstParamList(), myLevel_));
    }

  HYMLS_TEST(Label()+Teuchos::toString(myLevel_),
    isDDcorrect(*Teuchos::rcp_dynamic_cast<const Epetra_CrsMatrix>(matrix_),
      *hid_),__FILE__,__LINE__);

#ifdef HYMLS_TESTING
  this->Visualize("hid_data_deb.m",false);
  // schur preconditioner will do the same thing,
  // so the hid_data_deb.m file is always written,
  // even if the program crashes before the end of
  // the Compute() phase.
#endif

  // this is an idea to make the "HyperCube" object
  // statically available and using it to query things
  // like "idle CPU cores on node" dynamically

  /*
    if (HYMLS::ProcTopo==Teuchos::null)
    {
    HYMLS::Tools::Error("static object HYMLS::ProcTopo not constructed!");
    }
    int active = num_sd>0 ? 1:0;
    // this is just for figuring
    // out how many threads we can
    // use locally, it does not *change*
    // the process layout
    ProcTopo->setActive(myLevel_,active);
  */

  // Obtain a map with overlap between processors from the overlapping
  // partitioner which we need for the A12/A21 subdomain blocks
  rowMap_ = hid_->GetOverlappingMap();

#if defined(HYMLS_STORE_MATRICES) || defined(HYMLS_TESTING)
  MatrixUtils::Dump(*rangeMap_,"originalMap"+Teuchos::toString(myLevel_)+".txt");
  if (!rangeMap_->SameAs(*rowMap_))
    {
    MatrixUtils::Dump(*rowMap_,"reorderedMap"+Teuchos::toString(myLevel_)+".txt");
    }
#endif

  importer_=Teuchos::rcp(new Epetra_Import(*rowMap_,*rangeMap_));

  // Construct the matrix blocks we need for the Schur complement
  A11_ = Teuchos::rcp(new MatrixBlock(hid_,
      HierarchicalMap::Interior, HierarchicalMap::Interior, myLevel_));
  A12_ = Teuchos::rcp(new MatrixBlock(hid_,
      HierarchicalMap::Interior, HierarchicalMap::Separators, myLevel_));
  A21_ = Teuchos::rcp(new MatrixBlock(hid_,
      HierarchicalMap::Separators, HierarchicalMap::Interior, myLevel_));
  A22_ = Teuchos::rcp(new MatrixBlock(hid_,
      HierarchicalMap::Separators, HierarchicalMap::Separators, myLevel_));

  Teuchos::RCP<Teuchos::ParameterList> sd_list = Teuchos::rcp(new
    Teuchos::ParameterList(PL().sublist("Sparse Solver")));

  // Initialize the subdomain solvers for the A11 block
  CHECK_ZERO(A11_->InitializeSubdomainSolvers(sdSolverType_, sd_list, numThreadsSD_));

  HYMLS_DEBUG("Create Schur-complement");

  Epetra_Map const &map2 = A22_->RowMap();

  // construct the Schur-complement operator (no computations, just
  // pass in pointers of the LU's)
  Schur_ = Teuchos::rcp(new SchurComplement(
      A11_, A12_, A21_, A22_, myLevel_));
  Tools::out() << "=============================="<<std::endl;
  Tools::out() << "LEVEL "<< myLevel_<<std::endl;
  Tools::out() << "SIZE OF A: "<< matrix_->NumGlobalRows64()<<std::endl;
  Tools::out() << "SIZE OF S: "<< map2.NumGlobalElements64()<<std::endl;
  if (sdSolverType_=="Dense")
    {
    Tools::out() << "*** USING DENSE SUBDOMAIN SOLVERS ***"<<std::endl;
    }

  Tools::out() << "=============================="<<std::endl;

  if (myLevel_ < maxLevel_)
    {
    HYMLS_DEBUG("Construct schur-preconditioner");
    Teuchos::RCP<Epetra_Vector> testVector = CreateTestVector();
    schurPrec_ = Teuchos::rcp(new SchurPreconditioner(Schur_,hid_,
        getMyNonconstParamList(), myLevel_, testVector));

    CHECK_ZERO(schurPrec_->Initialize());
    }

  // create Belos' view of the Schur-complement problem
  schurRhs_=Teuchos::rcp(new Epetra_Vector(map2));
  schurSol_=Teuchos::rcp(new Epetra_Vector(map2));
  schurSol_->PutScalar(0.0);

  initialized_ = true;
  computed_ = false;
  numInitialize_++;
  timeInitialize_+=time_->ElapsedTime();

  return 0;
  }

// Returns true if the  preconditioner has been successfully initialized, false otherwise.
bool Preconditioner::IsInitialized() const {return initialized_;}

// Computes all it is necessary to apply the preconditioner.
int Preconditioner::Compute()
  {
  HYMLS_LPROF(label_,"Compute");
  if (!IsInitialized())
    {
    // the user should normally call Initialize before Compute
    Tools::Warning("HYMLS::Preconditioner not initialized. I'll do it for you.",
      __FILE__,__LINE__);
    this->Initialize();
    }

  time_->ResetStartTime();

    {
    HYMLS_LPROF2(label_, "matrix block computation");

    TransformMatrix();

    int MaxNumEntriesPerRow = matrix_->MaxNumEntries();
    Teuchos::RCP<const Epetra_CrsMatrix> Acrs = Teuchos::null;
    Acrs = Teuchos::rcp_dynamic_cast<const Epetra_CrsMatrix>(matrix_);

    if (Teuchos::is_null(Acrs))
      {
      Tools::Error("Currently requires an Epetra_CrsMatrix!",__FILE__,__LINE__);
      }

    HYMLS_DEBUG("Reorder global matrix");
    Teuchos::RCP<Epetra_CrsMatrix> reorderedMatrix =
      Teuchos::rcp(new Epetra_CrsMatrix(Copy, *rowMap_, MaxNumEntriesPerRow));

    CHECK_ZERO(reorderedMatrix->Import(*Acrs, *importer_, Insert));
    CHECK_ZERO(reorderedMatrix->FillComplete());

    // Compute the A12, A21, A22 blocks
    CHECK_ZERO(A12_->Compute(Acrs, reorderedMatrix));
    CHECK_ZERO(A21_->Compute(Acrs, reorderedMatrix));
    CHECK_ZERO(A22_->Compute(Acrs, reorderedMatrix));

#ifdef HYMLS_STORE_MATRICES
    MatrixUtils::Dump(A12_->Block()->RowMap(), "Precond"+Teuchos::toString(myLevel_)+"_Map1.txt");
    MatrixUtils::Dump(A21_->Block()->RowMap(), "Precond"+Teuchos::toString(myLevel_)+"_Map2.txt");

    MatrixUtils::Dump(*A12_->Block(), "Precond"+Teuchos::toString(myLevel_)+"_A12.txt");
    MatrixUtils::Dump(*A21_->Block(), "Precond"+Teuchos::toString(myLevel_)+"_A21.txt");
    MatrixUtils::Dump(*A22_->Block(), "Precond"+Teuchos::toString(myLevel_)+"_A22.txt");

    // note: the Compute and ComputeSubdomainSolvers functions both extract the matrix block,
    // so normally we don't need to call Compute() for A11
    CHECK_ZERO(A11_->Compute(Acrs, reorderedMatrix));
    MatrixUtils::Dump(*A11_->Block(), "Precond"+Teuchos::toString(myLevel_)+"_A11.txt");

#endif

    CHECK_ZERO(A11_->ComputeSubdomainSolvers(reorderedMatrix));

#ifdef HYMLS_TESTING
    Tools::out() << "Preconditioner level " << myLevel_ << ", doFmatTests=" << Tester::doFmatTests_ << std::endl;
    if (Tester::doFmatTests_)
      {
      // explicitly construct the SC and check wether it is an F-matrix
      Teuchos::RCP<Epetra_FECrsMatrix> TestSC =
        Teuchos::rcp(new Epetra_FECrsMatrix(Copy, A22_->RowMap(), Matrix().MaxNumEntries()));
      CHECK_ZERO(Schur_->Construct(TestSC));

#ifdef HYMLS_STORE_MATRICES
      HYMLS::MatrixUtils::Dump(*TestSC, "SchurComplement" + Teuchos::toString(myLevel_) + "_noDrop.txt");
#endif

      // this is usually done in Construct(), but not if we pass in a pointer ourselves.
      // We need a new pointer because DropByValue creates a CrsMatrix, not FECrsMatrix.
      Teuchos::RCP<Epetra_CrsMatrix> TestSC_crs = MatrixUtils::DropByValue(TestSC, HYMLS_SMALL_ENTRY);

      // Free some memory since these tests use huge amounts
      TestSC = Teuchos::null;

      HYMLS_TEST(Label(), isFmatrix(*TestSC_crs), __FILE__, __LINE__);

#ifdef HYMLS_STORE_MATRICES
      HYMLS::MatrixUtils::Dump(*TestSC_crs, "SchurComplement" + Teuchos::toString(myLevel_) + ".txt");
#endif
      }
#endif
    }

  if (myLevel_ >= maxLevel_)
    {
    int nzest = 32;
    if (hid_ != Teuchos::null && hid_->NumMySubdomains() > 0)
      nzest = hid_->NumSeparatorElements(0);

    Teuchos::RCP<Epetra_FECrsMatrix> matrix = Teuchos::rcp(new
      Epetra_FECrsMatrix(Copy, Schur_->OperatorDomainMap(), nzest));

    CHECK_ZERO(Schur_->Construct(matrix));

    schurPrec_ = Teuchos::rcp(new CoarseSolver(
        MatrixUtils::DropByValue(matrix, HYMLS_SMALL_ENTRY), myLevel_));
    CHECK_ZERO(schurPrec_->SetParameters(PL()));
    CHECK_ZERO(schurPrec_->Initialize());
    }

  CHECK_ZERO(ComputeBorder());

  CHECK_ZERO(schurPrec_->Compute());

  computed_ = true;
  timeCompute_ += time_->ElapsedTime();
  numCompute_++;

  if (PL().get("Visualize Solver",false)==true)
    {
    Tools::out() << "MATLAB file for visualizing the solver is written to hid_data.m" << std::endl;
    this->Visualize("hid_data.m");
    }

  return 0;
  }

int Preconditioner::ComputeBorder()
  {
  if (!HaveBorder())
    return 0;

  int m = V_->NumVectors();

  Epetra_Import const &import1 = A12_->Importer();
  Epetra_Import const &import2 = A21_->Importer();

  Epetra_Map const &map1 = A12_->RowMap();
  Epetra_Map const &map2 = A21_->RowMap();

  borderV1_ = Teuchos::rcp(new Epetra_MultiVector(map1, m));
  borderV2_ = Teuchos::rcp(new Epetra_MultiVector(map2, m));

  CHECK_ZERO(borderV1_->Import(*V_, import1, Insert));
  CHECK_ZERO(borderV2_->Import(*V_, import2, Insert));

  if (V_.get() == W_.get())
    {
    borderW1_ = borderV1_;
    borderW2_ = borderV2_;
    }
  else
    {
    borderW1_ = Teuchos::rcp(new Epetra_MultiVector(map1, m));
    borderW2_ = Teuchos::rcp(new Epetra_MultiVector(map2, m));
    CHECK_ZERO(borderW1_->Import(*W_, import1, Insert));
    CHECK_ZERO(borderW2_->Import(*W_, import2, Insert));
    }

  // Compute the border for the Schur-complement
  borderQ1_= Teuchos::rcp(new Epetra_MultiVector(map1, m));
  borderSchurV_ = Teuchos::rcp(new Epetra_MultiVector(map2, m));
  borderSchurW_ = Teuchos::rcp(new Epetra_MultiVector(map2, m));
  borderSchurC_ = Teuchos::rcp(new Epetra_SerialDenseMatrix(m, m));

  CHECK_ZERO(A11_->ApplyInverse(*borderV1_, *borderQ1_));
  CHECK_ZERO(A21_->Apply(*borderQ1_, *borderSchurV_));
  CHECK_ZERO(borderSchurV_->Update(1.0, *borderV2_, -1.0));

  // borderSchurW is given by W2 - (A11\A12)'W1
  // We use the formulation W2 - A12'(A11'\W1)
  Epetra_MultiVector w1tmp(map1, m);
  CHECK_ZERO(A11_->SetUseTranspose(true));
  CHECK_ZERO(A11_->ApplyInverse(*borderW1_, w1tmp));
  CHECK_ZERO(A11_->SetUseTranspose(false));

  CHECK_ZERO(A12_->SetUseTranspose(true));
  CHECK_ZERO(A12_->Apply(w1tmp, *borderSchurW_));
  CHECK_ZERO(A12_->SetUseTranspose(false));

  CHECK_ZERO(borderSchurW_->Update(1.0, *borderW2_, -1.0));

  CHECK_ZERO(DenseUtils::MatMul(*borderW1_, *borderQ1_, *borderSchurC_));
  CHECK_ZERO(borderSchurC_->Scale(-1.0));
  *borderSchurC_ += *C_;

  Teuchos::RCP<BorderedOperator> borderedPrec =
    Teuchos::rcp_dynamic_cast<BorderedOperator>(schurPrec_);
  if (Teuchos::is_null(borderedPrec))
    {
    HYMLS::Tools::Error("No bordered interface specified for the Schur complement solver", __FILE__, __LINE__);
    }

  CHECK_ZERO(borderedPrec->SetBorder(borderSchurV_, borderSchurW_, borderSchurC_));

  return 0;
  }

// Returns true if the  preconditioner has been successfully computed, false otherwise.
bool Preconditioner::IsComputed() const {return computed_;}

// Applies the preconditioner to vector B, returns the result in X.
int Preconditioner::ApplyInverse(const Epetra_MultiVector& B,
  Epetra_MultiVector& X) const
  {
  HYMLS_LPROF(label_,"ApplyInverse");
  Epetra_SerialDenseMatrix S, T;
  if (HaveBorder())
    {
    CHECK_ZERO(T.Shape(V_->NumVectors(), B.NumVectors()));
    CHECK_ZERO(S.Shape(V_->NumVectors(), X.NumVectors()));
    }
  return ApplyInverse(B, T, X, S);
  }

// Returns a pointer to the matrix to be preconditioned.
const Epetra_RowMatrix& Preconditioner::Matrix() const {return *matrix_;}

//TODO: the flops-counters currently do not include anything inside Belos

double Preconditioner::InitializeFlops() const
  {
  // the total number of flops is computed each time InitializeFlops() is
  // called. This is becase I also have to add the contribution from each
  // container.
  double total = flopsInitialize_;

  if (A11_ != Teuchos::null)
    {
    total += A11_->InitializeFlops();
    total += A12_->InitializeFlops();
    total += A21_->InitializeFlops();
    total += A22_->InitializeFlops();
    }

  if (schurPrec_!=Teuchos::null)
    {
    total += schurPrec_->InitializeFlops();
    }
  return total;
  }

double Preconditioner::ComputeFlops() const
  {
  double total = flopsCompute_;

  if (Schur_ != Teuchos::null)
    {
    total += Schur_->ComputeFlops();
    }

  if (A11_ != Teuchos::null)
    {
    total += A11_->ComputeFlops();
    total += A12_->ComputeFlops();
    total += A21_->ComputeFlops();
    total += A22_->ComputeFlops();
    }

  if (schurPrec_ != Teuchos::null)
    {
    total += schurPrec_->ComputeFlops();
    }
  return total;
  }

double Preconditioner::ApplyInverseFlops() const
  {
  double total = flopsApplyInverse_;

  if (Schur_!=Teuchos::null)
    {
    total += Schur_->ApplyFlops();
    }

  if (A11_ != Teuchos::null)
    {
    total += A11_->ApplyInverseFlops();
    total += A12_->ApplyFlops();
    total += A21_->ApplyFlops();
    total += A22_->ApplyFlops();
    }

  if (schurPrec_!=Teuchos::null)
    {
    total +=schurPrec_->ApplyInverseFlops();
    }
  return(total);
  }


// Computes the condition number estimate, returns its value.
double Preconditioner::Condest(const Ifpack_CondestType CT,
  const int MaxIters,
  const double Tol,
  Epetra_RowMatrix* Matrix)
  {
  Tools::Warning("not implemented!",__FILE__,__LINE__);
  return -1.0; // not implemented.
  }

// Returns the computed condition number estimate, or -1.0 if not computed.
double Preconditioner::Condest() const
  {
  Tools::Warning("not implemented!",__FILE__,__LINE__);
  return -1.0;
  }


// Returns the number of calls to Initialize().
int Preconditioner::NumInitialize() const {return numInitialize_;}

// Returns the number of calls to Compute().
int Preconditioner::NumCompute() const {return numCompute_;}

// Returns the number of calls to ApplyInverse().
int Preconditioner::NumApplyInverse() const {return numApplyInverse_;}

// Returns the time spent in Initialize().
double Preconditioner::InitializeTime() const {return timeInitialize_;}

// Returns the time spent in Compute().
double Preconditioner::ComputeTime() const {return timeCompute_;}

// Returns the time spent in ApplyInverse().
double Preconditioner::ApplyInverseTime() const {return timeApplyInverse_;}


// Prints basic information on iostream. This function is used by operator<<.
std::ostream& Preconditioner::Print(std::ostream& os) const
  {
  HYMLS_LPROF2(label_,"Print");
  os << Label() << std::endl;
  if (IsInitialized())
    {
    os << "+++++++++++++++++++++++++++++++++"<<std::endl;
    os << "+ Domain Decomposition object:  +"<<std::endl;
    os << "+++++++++++++++++++++++++++++++++"<<std::endl;
    os << *hid_;
    os << "+++++++++++++++++++++++++++++++++"<<std::endl;
    }
  else
    {
    os << " ... not initialized ..."<<std::endl;
    }

  if (IsComputed())
    {
    os << "+++++++++++++++++++++++++++++++++"<<std::endl;
    os << "+ Factorization info:           +"<<std::endl;
    os << "+++++++++++++++++++++++++++++++++"<<std::endl;
    os << "+ NOT IMPLEMENTED.              +"<<std::endl;
    os << "+++++++++++++++++++++++++++++++++"<<std::endl;
    }
  else
    {
    os << " ... not computed ..."<<std::endl;
    }
  return os;
  }

void Preconditioner::Visualize(std::string mfilename, bool no_recurse) const
  {
  HYMLS_LPROF2(label_,"Visualize");
  if ( (comm_->MyPID()==0) && (myLevel_==0))
    {
    std::ofstream ofs(mfilename.c_str(),std::ios::out);
    ofs << std::endl;
    ofs.close();
    }
  comm_->Barrier();
  std::ofstream ofs(mfilename.c_str(),std::ios::app);
  comm_->Barrier();
  ofs << *hid_<<std::endl;
  ofs.close();
#ifdef HYMLS_STORE_MATRICES
  Teuchos::RCP<const Epetra_CrsMatrix> A = Teuchos::rcp_dynamic_cast
    <const Epetra_CrsMatrix>(matrix_);
  if (A!=Teuchos::null) MatrixUtils::Dump(*A,"matrix"+Teuchos::toString(myLevel_)+".txt");
#endif

  Teuchos::RCP<const SchurPreconditioner> schurPrec =
    Teuchos::rcp_dynamic_cast<const SchurPreconditioner>(schurPrec_);
  if (schurPrec != Teuchos::null && !no_recurse)
    {
    schurPrec->Visualize(mfilename);
    }
  }

Teuchos::RCP<Epetra_Vector> Preconditioner::CreateTestVector()
  {
  if (testVector_==Teuchos::null)
    {
    testVector_=Teuchos::rcp(new Epetra_Vector(*rangeMap_));
    testVector_->PutScalar(1.0);
    }
  else
    {
    if (testVector_->Map().SameAs(*rangeMap_)==false)
      {
      Tools::Error("incompatible maps found!",__FILE__,__LINE__);
      }
    }

  Epetra_Map const &map2 = A22_->RowMap();

  Epetra_Vector tmpVec(*rowMap_);
  Teuchos::RCP<Epetra_Vector> testVector2 = Teuchos::rcp(new Epetra_Vector(map2));
  Teuchos::RCP<Epetra_Import> import2 = Teuchos::rcp(new Epetra_Import(map2, *rowMap_));
  CHECK_ZERO(tmpVec.Import(*testVector_,*importer_,Insert));
  CHECK_ZERO(testVector2->Import(tmpVec,*import2,Insert));

#ifdef STORE_TESTVECTOR
  Teuchos::RCP<Epetra_Import> importer = Teuchos::rcp(new Epetra_Import(Teuchos::rcp_dynamic_cast<const Epetra_CrsMatrix>(matrix_)->DomainMap(), *rangeMap_));
  Epetra_Vector tmpVec2(Teuchos::rcp_dynamic_cast<const Epetra_CrsMatrix>(matrix_)->DomainMap());
  tmpVec2.Import(*testVector_, *importer, Insert);

  Epetra_Vector out(*rangeMap_);
  matrix_->Multiply(false, tmpVec2, out);
  MatrixUtils::Dump(*testVector_, "testVec"+Teuchos::toString(myLevel_)+".txt");
  MatrixUtils::Dump(out, "multVec"+Teuchos::toString(myLevel_)+".txt");
  MatrixUtils::Dump(*Teuchos::rcp_dynamic_cast<const Epetra_CrsMatrix>(matrix_), "multMat"+Teuchos::toString(myLevel_)+".txt");

  MatrixUtils::Dump(*testVector2, "testVecDrop"+Teuchos::toString(myLevel_)+".txt");
#endif

  return testVector2;
  }

//////////////////////////////////
// BORDEREDSOLVER INTERFACE     //
//////////////////////////////////

// add a border to the preconditioner

// Implementation details
// ----------------------
//
// Let the original preconditioner be
//
// |A11 A12|      |A11  0 ||I A11\A12|
// |A21 A22|   ~= |A21  S ||0   I    |, S=A22 - A21*A11\A12,
//
// then the bordered variant will be
//
// |A11 A12 V1|      |A11    0              0       | |I A11\A12 Q1|
// |A21 A22 V2|   ~= |A21    S            V2-A21*Q1 | |0    I     0|
// |W1' W2' C |      |W1' W2'-W1'A11\A12   C-W1'Q1  | |0    0     I|, Q1=A11\V1.
//
// The lower right 2x2 block is the new 'bordered Schur Complement' and is handled by class
// SchurPreconditioner.
//
int Preconditioner::SetBorder(
  Teuchos::RCP<const Epetra_MultiVector> V,
  Teuchos::RCP<const Epetra_MultiVector> W,
  Teuchos::RCP<const Epetra_SerialDenseMatrix> C)
  {
  HYMLS_LPROF2(label_,"SetBorder");

  V_ = Teuchos::null;
  W_ = Teuchos::null;
  C_ = Teuchos::null;

  if (V == Teuchos::null)
    {
    borderSchurV_ = Teuchos::null;
    borderSchurW_ = Teuchos::null;
    borderSchurC_ = Teuchos::null;
    borderQ1_ = Teuchos::null;

    if (schurPrec_ == Teuchos::null)
      return 0;

    Teuchos::RCP<BorderedOperator> borderedPrec =
      Teuchos::rcp_dynamic_cast<BorderedOperator>(schurPrec_);
    if (Teuchos::is_null(borderedPrec))
      {
      HYMLS::Tools::Error("No bordered interface specified for the Schur complement solver", __FILE__, __LINE__);
      }

    CHECK_ZERO(borderedPrec->SetBorder(borderSchurV_, borderSchurW_, borderSchurC_));

    // Compute has to be called after setting the border, so make sure this happens.
    computed_ = false;

    return 0;
    }

  if (!V->Map().SameAs(OperatorRangeMap()) || (W != Teuchos::null &&
      !V->Map().SameAs(W->Map())))
    {
    Tools::Error("incompatible maps found", __FILE__, __LINE__);
    }

  int m = V->NumVectors();

  V_ = V;
  W_ = W;
  C_ = C;

  if (W == Teuchos::null)
    {
    W_ = V_;
    }

  if (W_->NumVectors() != m)
    {
    Tools::Error("Bordering: V and W must have same number of columns",
      __FILE__, __LINE__);
    }

  if (C == Teuchos::null)
    {
    C_ = Teuchos::rcp(new Epetra_SerialDenseMatrix(m, m));
    }

  if (C_->N() != C_->M() || C_->N() != m)
    {
    Tools::Error("Bordering: C block must be square and compatible with V and W",
      __FILE__, __LINE__);
    }

    // Compute has to be called after setting the border, so make sure this happens.
    computed_ = false;

  return 0;
  }

// if a border has been added, apply [Y T]' = [K V; W' O] [X S]'
int Preconditioner::Apply(const Epetra_MultiVector& X, const Epetra_SerialDenseMatrix& S,
  Epetra_MultiVector& Y,       Epetra_SerialDenseMatrix& T) const
  {
  // we don't need this for now
  Tools::Error("not implemented",__FILE__,__LINE__);
  return -99;
  }

// if a border has been added, apply [X S]' = [K V; W' O]\[Y T]'
int Preconditioner::ApplyInverse(const Epetra_MultiVector& B, const Epetra_SerialDenseMatrix& T,
  Epetra_MultiVector& X,       Epetra_SerialDenseMatrix& S) const
  {
  numApplyInverse_++;
  time_->ResetStartTime();

  if (!IsComputed())
    {
    HYMLS::Tools::Error("The preconditioner has not yet been computed.", __FILE__, __LINE__);
    }

#ifdef HYMLS_DEBUGGING
  if (dumpVectors_)
    {
    MatrixUtils::Dump(B, "Preconditioner"+Teuchos::toString(myLevel_)+"_Rhs.txt");
    }
#endif

  int numvec = X.NumVectors();

  Epetra_Import const &import1 = A12_->Importer();
  Epetra_Import const &import2 = A21_->Importer();

  Epetra_Map const &map1 = A12_->RowMap();
  Epetra_Map const &map2 = A21_->RowMap();

  Epetra_MultiVector x1(map1, numvec);
  Epetra_MultiVector x2(map2, numvec);

  Epetra_MultiVector b1(map1, numvec);
  Epetra_MultiVector b2(map2, numvec);

  Epetra_MultiVector y1(map1, numvec);
  Epetra_MultiVector y2(map2, numvec);

  // We first import B into the parts of B belonging to their blocks
  if (T_ != Teuchos::null)
    {
    Epetra_MultiVector BT(B);
    Tools::StartTiming("TransformMatix: MV transform 1");
    CHECK_ZERO(T_->Multiply(true, B, BT));
    Tools::StopTiming("TransformMatix: MV transform 1");

    CHECK_ZERO(b1.Import(BT, import1, Insert));
    CHECK_ZERO(b2.Import(BT, import2, Insert));
    }
  else
    {
    CHECK_ZERO(b1.Import(B, import1, Insert));
    CHECK_ZERO(b2.Import(B, import2, Insert));
    }

  // We want to compute
  // A11*x1 + A12*x2 = b1
  // A21*x1 + A22*x2 = b2
  // which results in solving
  // S*x2 = b2 - A21*A11\b1
  // x1 = -A11\A12*x2 + A11\b1
  // where S is the Schur complement

  // We first compute x1 = A11\b1, which we keep for later
  CHECK_ZERO(A11_->ApplyInverse(b1, x1));

  // Now we compute y2 = A21*A11\b1
  CHECK_ZERO(A21_->Apply(x1, y2));

  // We now compute the right-hand side for the Schur complement solve
  if (schurRhs_->NumVectors() != X.NumVectors())
    {
    schurRhs_ = Teuchos::rcp(new Epetra_MultiVector(map2, X.NumVectors()));
    schurSol_ = Teuchos::rcp(new Epetra_MultiVector(map2, X.NumVectors()));
    schurSol_->PutScalar(0.0);
    }
  CHECK_ZERO(schurRhs_->Update(1.0, b2, -1.0, y2, 0.0));

  // We now compute the border in case it is present
  Epetra_SerialDenseMatrix q;
  if (W_!=Teuchos::null)
    {
    CHECK_ZERO(DenseUtils::MatMul(*borderW1_, x1, q));
    CHECK_ZERO(q.Scale(-1));
    q += T;
    }

  Teuchos::RCP<BorderedOperator> borderedPrec =
    Teuchos::rcp_dynamic_cast<BorderedOperator>(schurPrec_);
  if (Teuchos::is_null(borderedPrec))
    {
    HYMLS::Tools::Error("No bordered interface specified for the Schur complement solver", __FILE__, __LINE__);
    }

  CHECK_ZERO(borderedPrec->ApplyInverse(*schurRhs_, q, *schurSol_, S));

  x2 = *schurSol_;

  // We have x2 now, so now we can compute x1. Remember that part of the solution
  // is already in there. We first compute y1=A12*x2
  CHECK_ZERO(A12_->Apply(x2, y1));

  // And now b1=A11\A12*x2. Note that we just use b1 as tmp variable here
  CHECK_ZERO(A11_->ApplyInverse(y1, b1));

  // And finally we update x1
  // this gives the final result [x1-y1; x2]
  CHECK_ZERO(x1.Update(-1.0, b1, 1.0));

  // Bordered stuff again
  if (borderQ1_!=Teuchos::null)
    {
    Teuchos::RCP<Epetra_MultiVector> ss = DenseUtils::CreateView(S);
    CHECK_ZERO(x1.Multiply('N', 'N', -1.0, *borderQ1_, *ss, 1.0));
    }

  // And now we import the result into X
  //'Zero' would disable repartitioning here (some
  // ranks may have a part of the vector but not
  // of the preconditioner), and
  //'Insert' would put the empty overlap nodes into
  // the other subdomains, so we need to zero out X
  // and 'Add' instead.
  CHECK_ZERO(X.PutScalar(0.0));
  CHECK_ZERO(X.Export(x1, import1, Add));
  CHECK_ZERO(X.Export(x2, import2, Add));
  if (T_ != Teuchos::null)
    {
    Tools::StartTiming("TransformMatix: MV transform 2");
    Epetra_MultiVector XT(X);
    CHECK_ZERO(T_->Multiply(false, XT, X));
    Tools::StopTiming("TransformMatix: MV transform 2");
    }

#ifdef HYMLS_DEBUGGING
  if (dumpVectors_)
    {
    MatrixUtils::Dump(X, "Preconditioner"+Teuchos::toString(myLevel_)+"_Sol.txt");
    dumpVectors_=numApplyInverse_>1?false: true;
    }
#endif
  timeApplyInverse_+=time_->ElapsedTime();
  return 0;
  }

int Preconditioner::TransformMatrix()
  {
  HYMLS_LPROF2(label_, "TransformMatix");

  if (!bgridTransform_)
    return 0;

  if (myLevel_ == 0)
    {
    Tools::StartTiming("TransformMatix: Construct T");
    Epetra_Map const &map = matrix_->RowMatrixRowMap();
    T_ = Teuchos::rcp(new Epetra_CrsMatrix(Copy, map, 2));
    int dof = PL("Problem").get("Degrees of Freedom", 1);
    hymls_gidx indices[2];
    double values[2];
    for (int i = 0; i < T_->NumMyRows(); i++)
      {
      int num = 1;
      double val = sqrt(0.5);
      hymls_gidx gid = map.GID64(i);

      indices[0] = gid;
      values[0] = 1.0;

      if (gid % dof == 0)
        {
        values[0] = val;
        indices[1] = gid+1;
        values[1] = -val;
        num = 2;
        }
      else if (gid % dof == 1)
        {
        values[0] = val;
        indices[1] = gid-1;
        values[1] = val;
        num = 2;
        }
      CHECK_ZERO(T_->InsertGlobalValues(gid, num, values, indices));
      }
    CHECK_ZERO(T_->FillComplete());
    Tools::StopTiming("TransformMatix: Construct T");

    Teuchos::RCP<const Epetra_CrsMatrix> crsMatrix =
      Teuchos::rcp_dynamic_cast<const Epetra_CrsMatrix>(matrix_);
    if (crsMatrix == Teuchos::null)
      Tools::Error("Stokes-B Preconditioner only supports Epetra_CrsMatrix", __FILE__, __LINE__);

#ifdef HYMLS_STORE_MATRICES
    MatrixUtils::Dump(*crsMatrix, "Matrix-Stokes-B.txt");
#endif
    Tools::StartTiming("Construct tmp matrices");

    Teuchos::RCP<Epetra_CrsMatrix> tmp1 = Teuchos::rcp(new Epetra_CrsMatrix(*crsMatrix));
    Teuchos::RCP<Epetra_CrsMatrix> tmp2 = Teuchos::rcp(new Epetra_CrsMatrix(Copy, map, 27));
    Teuchos::RCP<Epetra_CrsMatrix> tmp3 = Teuchos::rcp(new Epetra_CrsMatrix(Copy, map, 27));
    matrix_ = Teuchos::null;
    crsMatrix = Teuchos::null;
    Tools::StopTiming("Construct tmp matrices");

#ifdef HYMLS_STORE_MATRICES
    MatrixUtils::Dump(*tmp1, "ScaledMatrix-Stokes-B.txt");
    MatrixUtils::Dump(*T_, "TransformationMatrix-Stokes-B.txt");
#endif
    Tools::StartTiming("TransformMatix: MM 1");

    CHECK_ZERO(::EpetraExt::MatrixMatrix::Multiply(*T_, true, *tmp1, false, *tmp2));
    tmp1 = Teuchos::null;

    Tools::StopTiming("TransformMatix: MM 1");
    Tools::StartTiming("TransformMatix: MM 2");
    CHECK_ZERO(::EpetraExt::MatrixMatrix::Multiply(*tmp2, false, *T_, false, *tmp3));
    tmp2 = Teuchos::null;
    Tools::StopTiming("TransformMatix: MM 2");

    Tools::StartTiming("TransformMatix: Drop");
    matrix_ = MatrixUtils::DropByValue(tmp3);
    Tools::StopTiming("TransformMatix: Drop");

#ifdef HYMLS_STORE_MATRICES
    MatrixUtils::Dump(*tmp3, "Transformed-Stokes-B.txt");
#endif
    }
  return 0;
  }



  }//namespace
