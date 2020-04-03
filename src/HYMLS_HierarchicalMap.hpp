#ifndef HYMLS_HIERARCHICAL_MAP_H
#define HYMLS_HIERARCHICAL_MAP_H

#include "HYMLS_config.h"

#include "Teuchos_RCP.hpp"
#include "Teuchos_Array.hpp"
#include "Epetra_Map.h"

#include <iosfwd>
#include <string>

// forward declarations
class Epetra_Comm;

namespace HYMLS {

class InteriorGroup;
class SeparatorGroup;

//! class for hierarchically partitioned maps

/*! This class allows constructing Epetra_Maps which are sub-maps
  of a given base-map. The terminology we use comes from the
  HYMLS application of a map partitioned into
  - partitions (1 per proc)
  - subdomains (many per proc)
  - groups (several per subdomain

  The groups are again divided into 'interior' and 'separator' groups,
  the first (main) group of each subdomain is called interior
*/
class HierarchicalMap
  {

public:

  enum SpawnStrategy
    {
    Interior,       // retain only interior (level 0) nodes (no overlap).
    Separators,     // retain all separator elements as new 'interior'
                    // nodes, and keep separators between physical
                    // partitions (processors) as separators.
    LocalSeparators // retain all local separators, possibly regrouped.
    };

  // constructor - empty object
  HierarchicalMap(Teuchos::RCP<const Epetra_Map> baseMap,
    Teuchos::RCP<const Epetra_Map> baseOverlappingMap=Teuchos::null,
    int numMySubdomains=0,
    std::string label="HierarchicalMap",
    int level=1);

  //! destructor
  virtual ~HierarchicalMap();

  //! print domain decomposition to file
  std::ostream& Print(std::ostream& os) const;

  //! \name Functions to access the reordering defined by this class

  //@{

  //! get the local number of subdomains
  int NumMySubdomains() const;

  //! total number of interior nodes in subdomain sd
  int NumInteriorElements(int sd) const;

  //! total number of separator nodes in subdomain sd
  int NumSeparatorElements(int sd) const;

  //! The number of separator groups in subdomain sd
  int NumSeparatorGroups(int sd) const;

  //! The number of separator groups in subdomain sd
  int NumLinkedSeparatorGroups(int sd) const;

  //! returns the separator groups for a certain subdomain
  InteriorGroup const &GetInteriorGroup(int sd) const;

  //! returns the separator groups for a certain subdomain
  Teuchos::Array<SeparatorGroup> const &GetSeparatorGroups(int sd) const;

  //! returns the separator groups for a certain subdomain
  Teuchos::Array<Teuchos::Array<SeparatorGroup> > const &GetLinkedSeparatorGroups(int sd) const;
  //@}

  //! creates a 'next generation' object that retains certain nodes.

  /*!

    Currently this function allows doing the following:

    strat==Interior:    returns an object that has the same number of
    subdomains but only one group per subdomain,
    the interior nodes. The new object's Map() is
    a map without overlap that contains only the
    interior nodes of this object.

    strat==Separators: the new object contains all the local separator
    groups as new interior groups (each group forms
    the interior of exactly one subdomain). The last
    subdomain has a number of separator groups
    containing the off-processor separators connecting
    to subdomains in the original 'this' object.

    TODO: in the second case there may be a smarter implementation that
    retains more information about the non-local separators.

    strat=LocalSeparators: This object is used for constructing the
    orthogonal transform, it has only local sepa-
    rators.

    The 'Interior' object's Map() gives the variables to be eliminated
    in the first place.

    The 'Separator' object's Map() gives the map for the Schur-
    complement. Its indexing functions can be used to loop over the
    rows of a sparse matrix (new interior nodes) or its columns (new
    interior+separator nodes).

  */
  virtual Teuchos::RCP<const HierarchicalMap> Spawn(SpawnStrategy strat) const;

  //! spawn a map containing all Interior or all Separator nodes belonging to one subdomain,
  //! or All nodes belonging to one subdomain.
  Teuchos::RCP<const Epetra_Map> SpawnMap(int sd, SpawnStrategy strat) const;


  //!\name data member access
  //@{

  //!
  const Epetra_Comm& Comm() const {return baseMap_->Comm();}

  //!
  std::string Label() const {return label_;}

  //! get a reference to the non-overlapping map used inside this class
  const Epetra_Map& Map() const
    {
    return *baseMap_;
    }

  //! get a reference to the overlapping map used inside this class
  const Epetra_Map& OverlappingMap() const
    {
    return *overlappingMap_;
    }

  //!
  int Level() const {return myLevel_;}


  //! get a pointer to the non-overlapping map used inside this class
  Teuchos::RCP<const Epetra_Map> GetMap() const
    {
    return baseMap_;
    }

  //! get a reference to the overlapping map used inside this class
  Teuchos::RCP<const Epetra_Map> GetOverlappingMap() const
    {
    return overlappingMap_;
    }

protected:

  //@}

  //! add an interior group of GIDs to an existing subdomain.
  //! FillComplete() should not have been called.
  int AddInteriorGroup(int sd, InteriorGroup const &group);

  //! add a separator group of GIDs to an existing subdomain. Returns the group id
  //! of the new group. FillComplete() should not have been called.
  int AddSeparatorGroup(int sd, SeparatorGroup const &group);

  //! delete the map and all subdomains and groups that have been added so far
  int Reset(int num_sd);

  //! finalize the setup procedure by building the map
  int FillComplete();

  //! indicates if any more changes can be made (FillComplete() has been called)
  bool Filled() const {return overlappingMap_!=Teuchos::null;}

  //! label
  std::string label_;

  //! level ID
  int myLevel_;

  //! initial map (p0)
  Teuchos::RCP<const Epetra_Map> baseMap_;

  //! initial overlapping map from the previous level
  Teuchos::RCP<const Epetra_Map> baseOverlappingMap_;

private:

  //! overlapping map p1 (with minimal overlap between subdomains)
  Teuchos::RCP<const Epetra_Map> overlappingMap_;

  //! list of interior groups per subdomain
  Teuchos::RCP<Teuchos::Array<InteriorGroup> > interior_groups_;

  //! list of separator groups per subdomain
  Teuchos::RCP<Teuchos::Array<Teuchos::Array<SeparatorGroup> > > separator_groups_;

  //! list of separator groups per subdomain
  Teuchos::RCP<Teuchos::Array<Teuchos::Array<SeparatorGroup> > > unique_separator_groups_;

  //! list of separator groups per subdomain that are linked together,
  //! for instance because they are on the same separator.
  Teuchos::RCP<Teuchos::Array<Teuchos::Array<Teuchos::Array<SeparatorGroup> > > > linked_separator_groups_;

  //! array of spawned objects (so we avoid building the same thing over and over again)
  mutable Teuchos::Array<Teuchos::RCP<const HierarchicalMap> > spawnedObjects_;

  //! array of spawned maps
  mutable Teuchos::Array<Teuchos::Array<Teuchos::RCP<const Epetra_Map> > > spawnedMaps_;

  //! protected constructor - does not allow any more changes
  //! (FillComplete() has been called), this is used for spawning
  //! objects like a map with all separators etc.
  HierarchicalMap(
    Teuchos::RCP<const Epetra_Map> baseMap,
    Teuchos::RCP<const Epetra_Map> overlappingMap,
    Teuchos::RCP<Teuchos::Array<InteriorGroup> > interior_groups,
    Teuchos::RCP<Teuchos::Array<Teuchos::Array<SeparatorGroup> > > separator_groups,
    Teuchos::RCP<Teuchos::Array<Teuchos::Array<Teuchos::Array<SeparatorGroup> > > > linked_separator_groups,
    std::string label, int level);

  //! \name private member functions
  //! @{

  int LinkSeparators(
    Teuchos::RCP<Teuchos::Array<Teuchos::Array<SeparatorGroup> > > separator_groups,
    Teuchos::RCP<Teuchos::Array<Teuchos::Array<Teuchos::Array<SeparatorGroup> > > > linked_separator_groups) const;

  //!
  Teuchos::RCP<const HierarchicalMap> SpawnInterior() const;
  //!
  Teuchos::RCP<const HierarchicalMap> SpawnSeparators() const;
  //!
  Teuchos::RCP<const HierarchicalMap> SpawnLocalSeparators() const;

  //@}
  };

std::ostream & operator<<(std::ostream& os, const HierarchicalMap& h);

  }
#endif
