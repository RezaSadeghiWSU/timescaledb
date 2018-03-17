#ifndef TIMESCALEDB_SPACE_PARTITION_EXCLUDE_H
#define TIMESCALEDB_SPACE_PARTITION_EXCLUDE_H

#include <postgres.h>
#include "dimension.h"

typedef struct SpacePartitionExclude
{
	Dimension  *dim;
	int32		value;
} SpacePartitionExclude;

SpacePartitionExclude *space_partition_exclude_get(RestrictInfo *base_restrict_info, Query *query, Hypertable *hentry);

void space_partition_exclude_apply(SpacePartitionExclude *spe,
							  PlannerInfo *root,
							  Hypertable *ht,
							  Index main_table_child_index);


#endif							/* TIMESCALEDB_SPACE_PARTITION_EXCLUDE_H */
