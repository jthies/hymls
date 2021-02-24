#ifndef HYMLS_COMPLEX_SOLVER_H
#define HYMLS_COMPLEX_SOLVER_H

#include <complex>

#include "Teuchos_RCP.hpp"

#include "HYMLS_BaseSolver.hpp"

// forward declarations
class Epetra_MultiVector;
class Epetra_Operator;

namespace Belos
  {
template<typename, typename, typename>
class LinearProblem;
template<typename, typename, typename>
class SolverManager;
  }

namespace Teuchos
  {
class ParameterList;
  }

namespace HYMLS {

template<typename>
class ComplexVector;
template<typename, typename>
class ComplexOperator;

/*! iterative solver class, basically
   an Epetra wrapper for Belos extended with
   some bordering and deflation functionality.
*/
class ComplexSolver : public virtual BaseSolver
  {

  using BelosMultiVectorType = ComplexVector<Epetra_MultiVector>;
  using BelosOperatorType = ComplexOperator<Epetra_Operator, Epetra_MultiVector>;
  using BelosProblemType = Belos::LinearProblem<
    std::complex<double>, BelosMultiVectorType, BelosOperatorType>;
  using BelosSolverType = Belos::SolverManager<
    std::complex<double>, BelosMultiVectorType, BelosOperatorType>;

public:

  //!
  //! Constructor
  //!
  //! arguments: matrix, preconditioner and belos params.
  //!
  ComplexSolver(Teuchos::RCP<const Epetra_Operator> K,
    Teuchos::RCP<Epetra_Operator> P,
    Teuchos::RCP<Teuchos::ParameterList> params,
    bool validate=true);

  //! destructor
  virtual ~ComplexSolver();

  //! set solver parameters (the list is the "HYMLS"->"Solver" sublist)
  virtual void setParameterList(const Teuchos::RCP<Teuchos::ParameterList>& params);

  //! set solver parameters (the list is the "HYMLS"->"Solver" sublist)
  //! The extra argument is so it can be used by the actual Solver class
  virtual void setParameterList(const Teuchos::RCP<Teuchos::ParameterList>& params,
    bool validateParameters);

  //! get a list of valid parameters for this object
  virtual Teuchos::RCP<const Teuchos::ParameterList> getValidParameters() const;

  //! set preconditioner for solve
  virtual void SetPrecond(Teuchos::RCP<Epetra_Operator> P);

  virtual void SetTolerance(double tol);

  //! Applies the preconditioner to vector X, returns the result in Y.
  virtual int ApplyInverse(const Epetra_MultiVector& X,
    Epetra_MultiVector& Y) const;

protected:

//@}

  //! label
  std::string label_;

  //! Belos preconditioner interface
  Teuchos::RCP<BelosOperatorType> belosPrecPtr_;

  //! Belos linear problem interface
  Teuchos::RCP<BelosProblemType> belosProblemPtr_;

  //! Belos solver
  Teuchos::RCP<BelosSolverType> belosSolverPtr_;

  };


}

#endif
