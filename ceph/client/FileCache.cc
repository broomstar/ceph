
#include "config.h"
#include "include/types.h"

#include "FileCache.h"
#include "osdc/ObjectCacher.h"


// flush/release/clean

void FileCache::flush_dirty(Context *onflush)
{
  if (oc->flush_set(inode.ino, onflush)) {
	onflush->finish(0);
	delete onflush;
  }
}

off_t FileCache::release_clean()
{
  return oc->release_set(inode.ino);
}

bool FileCache::is_cached()
{
  return oc->set_is_cached(inode.ino);
}

bool FileCache::is_dirty() 
{
  return oc->set_is_dirty_or_committing(inode.ino);
}

void FileCache::empty(Context *onempty)
{
  off_t unclean = release_clean();
  bool clean = oc->flush_set(inode.ino, onempty);
  assert(!unclean == clean);

  if (clean) {
	onempty->finish(0);
	delete onempty;
  }
}


// caps

void FileCache::set_caps(int caps, Context *onimplement) 
{
  if (onimplement) {
	assert(latest_caps & ~caps);  // we should be losing caps.
	caps_callbacks[caps].push_back(onimplement);
  }
  
  latest_caps = caps;
  check_caps();  
}


void FileCache::check_caps()
{
  int used = 0;
  if (num_reading) used |= CAP_FILE_RD;
  if (oc->set_is_cached(inode.ino)) used |= CAP_FILE_RDCACHE;
  if (num_writing) used |= CAP_FILE_WR;
  if (oc->set_is_dirty_or_committing(inode.ino)) used |= CAP_FILE_WRBUFFER;
  dout(10) << "check_caps used " << cap_string(used) << endl;

  // check callbacks
  map<int, list<Context*> >::iterator p = caps_callbacks.begin();
  while (p != caps_callbacks.end()) {
	if (~(p->first) & used) {
	  // implemented.
	  dout(10) << "used is " << cap_string(used) 
			   << ", caps " << cap_string(p->first) << " implemented, doing callback(s)" << endl;
	  finish_contexts(p->second);
	  map<int, list<Context*> >::iterator o = p;
	  p++;
	  caps_callbacks.erase(o);
	} else {
	  dout(10) << "used is " << cap_string(used) 
			   << ", caps " << cap_string(p->first) << " not yet implemented" << endl;
	  p++;
	}
  }
}



// read/write

int FileCache::read(off_t offset, size_t size, bufferlist& blist, Mutex& client_lock)
{
  int r = 0;

  // inc reading counter
  num_reading++;
  
  if (latest_caps & CAP_FILE_RDCACHE) {
	// read (and block)
	Cond cond;
	bool done = false;
	int rvalue = 0;
	C_Cond *onfinish = new C_Cond(&cond, &done, &rvalue);
	
	r = oc->file_read(inode, offset, size, &blist, onfinish);
	
	if (r == 0) {
	  // block
	  while (!done) 
		cond.Wait(client_lock);
	  r = rvalue;
	} else {
	  // it was cached.
	  delete onfinish;
	}
  } else {
	r = oc->file_atomic_sync_read(inode, offset, size, &blist, client_lock);
  }

  // dec reading counter
  num_reading--;

  if (num_reading == 0 && !caps_callbacks.empty()) 
	check_caps();

  return r;
}

void FileCache::write(off_t offset, size_t size, bufferlist& blist, Mutex& client_lock)
{
  // inc writing counter
  num_writing++;

  if (latest_caps & CAP_FILE_WRBUFFER) {   // caps buffered write?
	// wait? (this may block!)
	oc->wait_for_write(size, client_lock);

	// async, caching, non-blocking.
	oc->file_write(inode, offset, size, blist);
  } else {
	// atomic, synchronous, blocking.
	oc->file_atomic_sync_write(inode, offset, size, blist, client_lock);	  
  }	
	
  // dec writing counter
  num_writing--;
  if (num_writing == 0 && !caps_callbacks.empty())
	check_caps();
}

bool FileCache::all_safe()
{
  return !oc->set_is_dirty_or_committing(inode.ino);
}

void FileCache::add_safe_waiter(Context *c) 
{
  bool safe = oc->commit_set(inode.ino, c);
  if (safe) {
	c->finish(0);
	delete c;
  }
}
