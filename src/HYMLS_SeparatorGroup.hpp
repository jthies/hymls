#ifndef HYMLS_SEPARATOR_GROUP_H
#define HYMLS_SEPARATOR_GROUP_H

#include "Teuchos_RCP.hpp"
#include "Teuchos_Array.hpp"

#include "HYMLS_config.h"

namespace HYMLS
  {

class SeparatorGroup
  {
  Teuchos::RCP<Teuchos::Array<hymls_gidx> > nodes_;

public:
  SeparatorGroup();

  Teuchos::Array<hymls_gidx> &nodes();
  };

  }
#endif