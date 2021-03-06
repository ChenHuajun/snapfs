/*-------------------------------------------------------------------------
 *
 * snapfs.c
 *	  File system snapshots providing fast recovery
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/file/snapfs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#ifndef WIN32
#include <sys/mman.h>
#endif
#include <unistd.h>
#include <fcntl.h>

#include "miscadmin.h"
#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "common/file_perm.h"
#include "postmaster/bgwriter.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "storage/snapfs.h"
#include "storage/standbydefs.h"
#include "storage/bufmgr.h"
#include "utils/inval.h"
#include "utils/fmgrprotos.h"


SnapshotId sfs_backend_snapshot = SFS_INVALID_SNAPSHOT;
SnapshotId sfs_active_snapshot;
bool sfs_basebackup;

int
sfs_msync(SnapshotMap * map)
{
	if (!enableFsync)
		return 0;
#ifdef WIN32
	return FlushViewOfFile(map, sizeof(SnapshotMap)) ? 0 : -1;
#else
	return msync(map, sizeof(SnapshotMap), MS_SYNC);
#endif
}

SnapshotMap *
sfs_mmap(int md)
{
	SnapshotMap    *map;

	if (ftruncate(md, sizeof(SnapshotMap)) != 0)
	{
		return (SnapshotMap *) MAP_FAILED;
	}

#ifdef WIN32
	{
		HANDLE		mh = CreateSnapshotMapping((HANDLE)_get_osfhandle(md), NULL, PAGE_READWRITE,
										   0, (DWORD) sizeof(SnapshotMap), NULL);

		if (mh == NULL)
			return (SnapshotMap *) MAP_FAILED;

		map = (SnapshotMap *) MapViewOfFile(mh, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		CloseHandle(mh);
	}
	if (map == NULL)
		return (SnapshotMap *) MAP_FAILED;

#else
	map = (SnapshotMap *) mmap(NULL, sizeof(SnapshotMap), PROT_WRITE | PROT_READ, MAP_SHARED, md, 0);
#endif
	return map;
}

int
sfs_munmap(SnapshotMap * map)
{
#ifdef WIN32
	return UnmapViewOfFile(map) ? 0 : -1;
#else
	return munmap(map, sizeof(SnapshotMap));
#endif
}
/*
 * Safe read of file
 */
int
sfs_read_file(int fd, void *data, uint32 size)
{
	uint32		offs = 0;

	do
	{
		int			rc = (int) read(fd, (char *) data + offs, size - offs);

		if (rc <= 0)
		{
			if (rc == 0 || errno != EINTR)
				return rc;
		}
		else
			offs += rc;
	} while (offs < size);

	return size;
}

/*
 * Safe write of file
 */
bool
sfs_write_file(int fd, void const *data, uint32 size)
{
	uint32		offs = 0;

	do
	{
		int			rc = (int) write(fd, (char const *) data + offs, size - offs);

		if (rc <= 0)
		{
			if (errno != EINTR)
				return false;
		}
		else
			offs += rc;
	} while (offs < size);

	return true;
}


void
sfs_check_snapshot(SnapshotId snap_id)
{
	if (snap_id < ControlFile->oldest_snapshot || snap_id > ControlFile->recent_snapshot)
	{
		if (!SFS_KEEPING_SNAPSHOT())
			elog(ERROR, "Not keeping any snapshot");
		else
			elog(ERROR, "Invalid snapshot %d, existed snapshots %d..%d",
				 snap_id, ControlFile->oldest_snapshot, ControlFile->recent_snapshot);
	}
}

void
sfs_switch_to_snapshot(SnapshotId snap_id)
{
	if (snap_id != SFS_INVALID_SNAPSHOT)
		sfs_check_snapshot(snap_id);

	sfs_lock_database();

	if (!SFS_IN_SNAPSHOT())
		sfs_checkpoint();

	LWLockAcquire(DoAutovacuumLock, LW_EXCLUSIVE); /* prevenyt autovacuum in snapshot */
	ControlFile->active_snapshot = snap_id;
	LWLockRelease(DoAutovacuumLock);
	UpdateControlFile();

	DropSharedBuffers();
	InvalidateSystemCaches();
	CacheInvalidateRelcacheAll();

	sfs_unlock_database();
}

void
sfs_set_backend_snapshot(SnapshotId snap_id)
{
	if (snap_id != SFS_INVALID_SNAPSHOT)
		sfs_check_snapshot(snap_id);

	sfs_backend_snapshot = snap_id;

	DropLocalBuffers();
	InvalidateSystemCaches();
}

SnapshotId
sfs_make_snapshot(void)
{
	SnapshotId snap_id;

	if (SFS_IN_SNAPSHOT())
		elog(ERROR, "Can not perform operation inside snapshot");

	sfs_lock_database();
	sfs_checkpoint();

	snap_id = ++ControlFile->recent_snapshot;
	UpdateControlFile();

	sfs_unlock_database();

	sfs_xlog_insert(XLOG_MAKE_SNAPSHOT, snap_id);

	return snap_id;
}

/*
 * Prohibit any database updates
 */
void
sfs_lock_database(void)
{
	if (!InRecovery)
		ProcArrayLockDatabase();
}

void
sfs_unlock_database(void)
{
	if (!InRecovery)
		ProcArrayUnlockDatabase();
}

void
sfs_xlog_insert(int op, SnapshotId snapid)
{
	if (!InRecovery)
	{
		xl_snapshot xlrec;
		xlrec.snapid = snapid;
		XLogBeginInsert();
		XLogSetRecordFlags(XLOG_MARK_UNIMPORTANT);
		XLogRegisterData((char *) (&xlrec), sizeof(xlrec));
		XLogInsert(RM_STANDBY_ID, op);
	}
}


void
sfs_checkpoint(void)
{
	int flags = CHECKPOINT_IMMEDIATE | CHECKPOINT_FORCE | CHECKPOINT_WAIT | CHECKPOINT_FLUSH_ALL;
	RequestCheckpoint(flags);
}

static void
sfs_not_at_replica(void)
{
	if (RecoveryInProgress())
		elog(ERROR, "Operation is not possible at replica");
}

/*
 * SQL interface to snapshots
 */

Datum pg_make_snapshot(PG_FUNCTION_ARGS)
{
	sfs_not_at_replica();
	PG_RETURN_INT32(sfs_make_snapshot());
}

Datum pg_remove_snapshot(PG_FUNCTION_ARGS)
{
	SnapshotId snap_id = PG_GETARG_INT32(0);
	sfs_remove_snapshot(snap_id);
	PG_RETURN_VOID();
}

Datum pg_recover_to_snapshot(PG_FUNCTION_ARGS)
{
	SnapshotId snap_id = PG_GETARG_INT32(0);
	sfs_not_at_replica();
	sfs_recover_to_snapshot(snap_id);
	PG_RETURN_VOID();
}

Datum pg_switch_to_snapshot(PG_FUNCTION_ARGS)
{
	SnapshotId snap_id = PG_GETARG_INT32(0);
	sfs_not_at_replica();
	sfs_switch_to_snapshot(snap_id);
	PG_RETURN_VOID();
}

Datum pg_set_backend_snapshot(PG_FUNCTION_ARGS)
{
	SnapshotId snap_id = PG_GETARG_INT32(0);
	sfs_set_backend_snapshot(snap_id);
	PG_RETURN_VOID();
}

Datum pg_get_snapshot_size(PG_FUNCTION_ARGS)
{
	SnapshotId snap_id = PG_GETARG_INT32(0);
	int64 size = sfs_get_snapshot_size(snap_id);
	PG_RETURN_INT64(size);
}

Datum pg_get_backend_snapshot(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(sfs_backend_snapshot);
}

Datum pg_get_snapshot_timestamp(PG_FUNCTION_ARGS)
{
	SnapshotId snap_id = PG_GETARG_INT32(0);
    time_t t = sfs_get_snapshot_timestamp(snap_id);
	if (t == (time_t)-1)
		PG_RETURN_NULL();
	else
		PG_RETURN_TIMESTAMPTZ(time_t_to_timestamptz(t));
}

