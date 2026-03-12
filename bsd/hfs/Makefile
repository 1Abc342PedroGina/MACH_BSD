export MakeInc_cmd=${SRCROOT}/makedefs/MakeInc.cmd
export MakeInc_def=${SRCROOT}/makedefs/MakeInc.def
export MakeInc_rule=${SRCROOT}/makedefs/MakeInc.rule
export MakeInc_dir=${SRCROOT}/makedefs/MakeInc.dir

include $(MakeInc_cmd)
include $(MakeInc_def)

EXPORT_MI_DIR = hfs

DATAFILES = \
	BTreeScanner.h BTreesInternal.h BTreesPrivate.h \
	CatalogPrivate.h FileMgrInternal.h HFSUnicodeWrappers.h UCStringCompareData.h \
	hfs.h hfs_alloc_trace.h hfs_attrlist.h hfs_btreeio.h \
	hfs_catalog.h hfs_cnode.h hfs_cprotect.h hfs_dbg.h hfs_endian.h

DRIVERKIT_DATAFILES = \
	hfs_extents.h hfs_format.h hfs_fsctl.h hfs_hotfiles.h hfs_iokit.h

PRIVATE_DATAFILES = \
	hfs_journal.h

KERNELFILES = \
	hfs_kdebug.h hfs_macos_defs.h \
	hfs_mount.h hfs_quota.h hfs_unistr.h kext-config.h \
	rangelist.h

INSTALL_MD_LIST = ${DATAFILES}
INSTALL_DRIVERKIT_MD_LIST = ${DRIVERKIT_DATAFILES}
INSTALL_SF_MD_LCL_LIST = ${DATAFILES} ${PRIVATE_DATAFILES}

INSTALL_MD_DIR = i386

EXPORT_MD_LIST = ${KERNELFILES}

EXPORT_MD_DIR = i386

include $(MakeInc_rule)
include $(MakeInc_dir)
