/*
 * Rados Filesystem - A filesystem library based in librados
 *
 * Copyright (C) 2014 CERN, Switzerland
 *
 * Author: Joaquim Rocha <joaquim.rocha@cern.ch>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License at http://www.gnu.org/licenses/lgpl-3.0.txt
 * for more details.
 */

#include <sys/stat.h>
#include <sstream>

#include "FileIO.hh"
#include "FileInode.hh"
#include "FileInodePriv.hh"
#include "FilesystemPriv.hh"
#include "radosfscommon.h"

RADOS_FS_BEGIN_NAMESPACE

FileInodePriv::FileInodePriv(Filesystem *fs, const std::string &poolName,
                             const std::string &name, const size_t stripeSize)
  : fs(fs),
    name(name)
{
  PoolSP pool = fs->mPriv->getDataPoolFromName(poolName);
  size_t stripe = alignStripeSize(stripeSize, pool->alignment);

  if (pool)
    io = FileIOSP(new FileIO(fs, pool, name, stripe));
}

FileInodePriv::FileInodePriv(Filesystem *fs, PoolSP &pool,
                             const std::string &name, const size_t stripeSize)
  : fs(fs),
    name(name)
{
  size_t stripe = alignStripeSize(stripeSize, pool->alignment);

  if (pool)
    io = FileIOSP(new FileIO(fs, pool, name, stripe));
}

FileInodePriv::FileInodePriv(Filesystem *fs, FileIOSP fileIO)
  : fs(fs)
{
  setFileIO(fileIO);
}

FileInodePriv::~FileInodePriv()
{}

void
FileInodePriv::setFileIO(FileIOSP fileIO)
{
  io = fileIO;

  if (io)
    name = io->inode();
}

int
FileInodePriv::registerFile(const std::string &path, uid_t uid, gid_t gid,
                            int mode)
{
  const std::string parentDir = getParentDir(path, 0);
  std::string filePath = path;

  if (parentDir == "")
  {
    radosfs_debug("Error registering inode %s with file path %s . The file "
                  "path needs to be absolute (start with a '/').", name.c_str(),
                  path.c_str());
    return -EINVAL;
  }

  Stat parentStat;

  int ret = fs->mPriv->stat(parentDir, &parentStat);

  if (ret < 0)
  {
    if (ret == -EEXIST)
      radosfs_debug("Cannot register inode %s in path %s: The parent directory "
                    "does not exist. (Verify that the path is an absolute path "
                    "without any links in it)", name.c_str(), filePath.c_str());
    return ret;
  }

  if (S_ISLNK(parentStat.statBuff.st_mode))
  {
    radosfs_debug("Cannot register inode %s in path %s: Be sure to provide an "
                  "existing absolute path containing no links.", name.c_str(),
                  filePath.c_str());
    return -EINVAL;
  }

  if (!S_ISDIR(parentStat.statBuff.st_mode))
  {
    radosfs_debug("Error registering inode %s with file path %s . The parent "
                  "directory is a regular file.", name.c_str(),
                  filePath.c_str());
    return -EINVAL;
  }

  Stat fileStat;
  ret = fs->mPriv->stat(filePath, &fileStat);

  if (ret == 0)
    return -EEXIST;

  long int permOctal = DEFAULT_MODE_FILE;

  if (mode >= 0)
    permOctal = mode | S_IFREG;

  timespec spec;
  clock_gettime(CLOCK_REALTIME, &spec);

  fileStat.path = filePath;
  fileStat.translatedPath = io->inode();
  fileStat.pool = io->pool();
  fileStat.statBuff = parentStat.statBuff;
  fileStat.statBuff.st_uid = uid;
  fileStat.statBuff.st_gid = gid;
  fileStat.statBuff.st_mode = permOctal;
  fileStat.statBuff.st_ctim = spec;
  fileStat.statBuff.st_ctime = spec.tv_sec;

  std::stringstream stream;
  stream << io->stripeSize();

  fileStat.extraData[XATTR_FILE_STRIPE_SIZE] = stream.str();

  ret = indexObject(&parentStat, &fileStat, '+');

  if (ret == -ECANCELED)
  {
    return -EEXIST;
  }

  return ret;
}

FileInode::FileInode(Filesystem *fs, const std::string &pool)
  : mPriv(new FileInodePriv(fs, pool, generateUuid(), fs->fileStripeSize()))
{}

FileInode::FileInode(Filesystem *fs, const std::string &name,
                     const std::string &pool)
  : mPriv(new FileInodePriv(fs, pool, name, fs->fileStripeSize()))
{}

FileInode::FileInode(Filesystem *fs, const std::string &name,
                     const std::string &pool, const size_t stripeSize)
  : mPriv(new FileInodePriv(fs, pool, name, stripeSize))
{}

FileInode::FileInode(Filesystem *fs, const std::string &pool,
                     const size_t stripeSize)
  : mPriv(new FileInodePriv(fs, pool, generateUuid(), stripeSize))
{}

FileInode::FileInode(FileInodePriv *priv)
  : mPriv(priv)
{}

FileInode::~FileInode()
{
  delete mPriv;
}

ssize_t
FileInode::read(char *buff, off_t offset, size_t blen)
{
  if (!mPriv->io)
    return -ENODEV;

  return mPriv->io->read(buff, offset, blen);
}

int
FileInode::write(const char *buff, off_t offset, size_t blen)
{
  return write(buff, offset, blen, false);
}

int
FileInode::write(const char *buff, off_t offset, size_t blen, bool copyBuffer)
{
  if (!mPriv->io)
    return -ENODEV;

  Stat stat;
  stat.pool = mPriv->io->pool();
  stat.translatedPath = mPriv->io->inode();

  updateTimeAsync(&stat, XATTR_MTIME);

  std::string opId;
  int ret = mPriv->io->write(buff, offset, blen, &opId, copyBuffer);

  {
    boost::unique_lock<boost::mutex> lock(mPriv->asyncOpsMutex);
    mPriv->asyncOps.push_back(opId);
  }

  return ret;
}

int
FileInode::writeSync(const char *buff, off_t offset, size_t blen)
{
  if (!mPriv->io)
    return -ENODEV;

  Stat stat;
  stat.pool = mPriv->io->pool();
  stat.translatedPath = mPriv->io->inode();

  updateTimeAsync(&stat, XATTR_MTIME);

  return mPriv->io->writeSync(buff, offset, blen);
}

int
FileInode::remove(void)
{
  if (!mPriv->io)
    return -ENODEV;

  return mPriv->io->remove();
}

int
FileInode::truncate(size_t size)
{
  if (!mPriv->io)
    return -ENODEV;

  Stat stat;
  stat.pool = mPriv->io->pool();
  stat.translatedPath = mPriv->io->inode();

  updateTimeAsync(&stat, XATTR_MTIME);

  return mPriv->io->truncate(size);
}

int
FileInode::sync()
{
  if (!mPriv->io)
    return -ENODEV;

  int ret = 0;
  boost::unique_lock<boost::mutex> lock(mPriv->asyncOpsMutex);

  std::vector<std::string>::iterator it;
  for (it = mPriv->asyncOps.begin(); it != mPriv->asyncOps.end(); it++)
  {
    ret = mPriv->io->sync(*it);
  }

  mPriv->asyncOps.clear();

  return ret;
}

std::string
FileInode::name() const
{
  if (!mPriv->io)
    return "";

  return mPriv->io->inode();
}

int
FileInode::registerFile(const std::string &path, uid_t uid, gid_t gid, int mode)
{
  if (!mPriv->io)
    return -ENODEV;

  if (path == "")
  {
    radosfs_debug("Error: path for registering inode %s is empty",
                  mPriv->name.c_str());
    return -EINVAL;
  }

  if (isDirPath(path))
  {
    radosfs_debug("Error attempting to register inode %s with directory path "
                  "%s. For registering an inode, it needs to be a file path "
                  "(no '/' in the end of it).", mPriv->name.c_str(),
                  path.c_str());

    return -EISDIR;
  }

  return mPriv->registerFile(path, uid, gid, mode);
}

RADOS_FS_END_NAMESPACE