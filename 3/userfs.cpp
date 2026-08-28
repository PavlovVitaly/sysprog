#include "userfs.h"

#include "rlist.h"

#include <stddef.h>
#include <string>
#include <sys/types.h>
#include <vector>
#include <string.h>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char memory[BLOCK_SIZE];
	/** A link in the block list of the owner-file. */
	rlist in_block_list = RLIST_LINK_INITIALIZER;

	/* PUT HERE OTHER MEMBERS */
};

struct file {
	/**
	 * Doubly-linked intrusive list of file blocks. Intrusiveness of the
	 * list gives you the full control over the lifetime of the items in the
	 * list without having to use double pointers with performance penalty.
	 */
	rlist blocks = RLIST_HEAD_INITIALIZER(blocks);
	/** How many file descriptors are opened on the file. */
	int refs = 0;
	/** File name. */
	std::string name;
	/** A link in the global file list. */
	rlist in_file_list = RLIST_LINK_INITIALIZER;

	/* PUT HERE OTHER MEMBERS */
	size_t size = 0;
	bool is_deleted = false;
};

/**
 * Intrusive list of all files. In this case the intrusiveness of the list also
 * grants the ability to remove items from any position in O(1) complexity
 * without having to know their iterator.
 */
static rlist file_list = RLIST_HEAD_INITIALIZER(file_list);

struct filedesc {
	file *atfile{};

	/* PUT HERE OTHER MEMBERS */
	block *block_ptr{};
	size_t block_num{};
	size_t offset{};
	int mode{};
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static std::vector<filedesc*> file_descriptors;

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

static file
*create_ufs_file(const std::string& filename){
	file *new_file = new file{};
	new_file->name = filename;
	rlist_add_tail_entry(&file_list, new_file, in_file_list);
	return new_file;
}

static file
*get_ufs_file(const std::string& filename){
	rlist *item{};
	rlist_foreach(item, &file_list){
		file* f = rlist_entry(item, struct file, in_file_list);
		if(f->name == filename && !f->is_deleted) return f;
	}
	return nullptr;
}

static filedesc
*create_ufs_filedesc(file *opened_file, int flags){
	filedesc *new_filedesc = new filedesc{};
	new_filedesc->mode = flags;
	block* blk = rlist_first_entry(&opened_file->blocks, struct block, in_block_list);
	new_filedesc->block_ptr = blk;
	new_filedesc->offset = 0;
	new_filedesc->block_num = 0;
	new_filedesc->atfile = opened_file;
	opened_file->refs++;
	return new_filedesc;
}

static void
create_init_block(filedesc *fd){
	block *new_block = new block{};
	memset(new_block->memory, 0, BLOCK_SIZE);
	rlist_add_tail_entry(&fd->atfile->blocks, new_block, in_block_list);
	fd->block_ptr = new_block;
	fd->block_num = 0;
	fd->offset = 0;
}

static void
add_new_empty_block(filedesc *fd){
	block *new_block = new block{};
	rlist_add_tail_entry(&fd->atfile->blocks, new_block, in_block_list);
	fd->block_ptr = new_block;
	fd->block_num++;
	fd->offset = 0;
}

static void
update_file_size(filedesc *fd){
	size_t new_file_size = static_cast<size_t>(BLOCK_SIZE) * fd->block_num + fd->offset;
	if(new_file_size > fd->atfile->size){
		fd->atfile->size = new_file_size;
	}
}

static void
actualize_file_descriptor(filedesc *desc){
	size_t sz = desc->block_num * static_cast<size_t>(BLOCK_SIZE) + desc->offset;
	if(sz > desc->atfile->size){
		desc->offset = desc->atfile->size % static_cast<size_t>(BLOCK_SIZE);
		desc->block_num = desc->atfile->size / static_cast<size_t>(BLOCK_SIZE);
	}

	if(!rlist_empty(&desc->atfile->blocks)){
		size_t cnt{};
		struct block *blk{};
		rlist_foreach_entry(blk, &desc->atfile->blocks, in_block_list){
			desc->block_ptr = blk;
			if(cnt == desc->block_num){
				break;
			}
			++cnt;
		}
	} else {
		desc->block_ptr = nullptr;
		desc->block_num = 0;
		desc->offset = 0;
	}
}

static filedesc 
*get_filedesc(int fd){
	if(fd < 0 || static_cast<size_t>(fd) >= file_descriptors.size()) {
		return nullptr;
	}
	filedesc *desc = file_descriptors[fd];
	if(!desc) return nullptr;
	
	actualize_file_descriptor(desc);
	return desc;
}

static int
find_free_filedescriptor(){
	for(int fd = 0; static_cast<size_t>(fd) < file_descriptors.size(); ++fd){
		if(file_descriptors[fd] == nullptr){
			return fd;
		}
	}
	return -1;
}

int
ufs_open(const char *filename, int flags)
{
	file* opened_file = get_ufs_file(filename);
	if((flags & UFS_CREATE) && !opened_file){
		opened_file = create_ufs_file(filename);
	}
	if(!opened_file) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

#if NEED_OPEN_FLAGS
	if(flags == UFS_CREATE) flags |= UFS_READ_WRITE;
#endif

	auto fd = find_free_filedescriptor();

	filedesc *desc = create_ufs_filedesc(opened_file, flags);
	if(fd < 0){
		file_descriptors.push_back(desc);
		return file_descriptors.size() - 1;
	}
	
	file_descriptors[fd] = desc;
	return fd;
}

static ssize_t
ufs_write_data_to_current_block(filedesc *desc, const char *buf, size_t size){
	if(size > BLOCK_SIZE - desc->offset) return -1;
	memcpy((desc->block_ptr->memory + desc->offset), buf, size);
	desc->offset += size;
	update_file_size(desc);
	return size;
}

static void
force_shift_to_next_block(filedesc *desc){
	if(desc->block_ptr->in_block_list.next == &desc->atfile->blocks){
		add_new_empty_block(desc);
		return;
	}

	block* blk = rlist_entry(desc->block_ptr->in_block_list.next, struct block, in_block_list);	
	desc->block_ptr = blk;
	desc->block_num++;
	desc->offset = 0;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	filedesc* desc = get_filedesc(fd);
	if(!desc) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

#if NEED_OPEN_FLAGS
	if(!(desc->mode & UFS_WRITE_ONLY) && desc->mode != 0){
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	if(desc->atfile->size == 0 && !desc->block_ptr && size != 0){
		create_init_block(desc);
	}

	size_t current_pos = desc->block_num * BLOCK_SIZE + desc->offset;
	if (current_pos + size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

	if(size <= BLOCK_SIZE - desc->offset){
		return ufs_write_data_to_current_block(desc, buf, size);
	}

	size_t total_size = ufs_write_data_to_current_block(desc, buf, BLOCK_SIZE - desc->offset);
	size_t new_size{};

	while(total_size < size){
		if (desc->offset == BLOCK_SIZE) force_shift_to_next_block(desc);
		size_t diff{size - total_size};
		new_size = static_cast<size_t>(BLOCK_SIZE - desc->offset) >= diff ? diff : static_cast<size_t>(BLOCK_SIZE - desc->offset);
		total_size += ufs_write_data_to_current_block(desc, buf + total_size, new_size);	
	}

	update_file_size(desc);
	return total_size;
}

static ssize_t
ufs_read_data_to_current_block(filedesc *desc, char *buf, size_t size){
	if(BLOCK_SIZE - desc->offset < size) return -1;
	if(size == 0) return 0;

	memcpy(buf, (desc->block_ptr->memory + desc->offset), size);
	desc->offset += size;
	return size;
}

static bool
soft_shift_to_next_block(filedesc *desc){
	if (desc->offset != BLOCK_SIZE) return true;
	if (desc->block_ptr->in_block_list.next == &desc->atfile->blocks) return false;
	desc->block_ptr = rlist_entry(desc->block_ptr->in_block_list.next, struct block, in_block_list);
	desc->block_num++;
	desc->offset = 0;
	return true;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	filedesc* desc = get_filedesc(fd);
	if(!desc) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

#if NEED_OPEN_FLAGS
	if(!(desc->mode & UFS_READ_ONLY) && desc->mode != 0){
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	if(!desc->block_ptr) return 0;

	ssize_t rsize = std::min(desc->atfile->size - (static_cast<size_t>(BLOCK_SIZE) * desc->block_num) - desc->offset, size);
	if(static_cast<ssize_t>(BLOCK_SIZE - desc->offset) >= rsize){
		return ufs_read_data_to_current_block(desc, buf, rsize);
	}

	if(rsize == 0) return 0;
	auto total_size = ufs_read_data_to_current_block(desc, buf, BLOCK_SIZE - desc->offset);

	while(total_size < rsize){
		if (desc->offset == BLOCK_SIZE) {
			if (!soft_shift_to_next_block(desc)) break;
		}
		ssize_t diff{rsize - total_size};
		size_t blk_sz = static_cast<ssize_t>(BLOCK_SIZE - desc->offset) >= diff ? diff : static_cast<size_t>(BLOCK_SIZE - desc->offset);
		total_size += ufs_read_data_to_current_block(desc, buf + total_size, blk_sz);
	}

	return total_size;
}

static void
delete_blocks_in_file(file *f){
	struct block *tmp{};
	struct block *blk{};
	rlist_foreach_entry_safe(blk, &f->blocks, in_block_list, tmp){
		rlist_del_entry(blk, in_block_list);
		delete blk;
	}
}

static void
delete_file(file *f){
	delete_blocks_in_file(f);
	delete f;
}

int
ufs_close(int fd)
{
	filedesc* desc = get_filedesc(fd);
	if(!desc) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	file *f = desc->atfile;
	f->refs--;
	
	delete desc;
	file_descriptors[fd] = nullptr;
	
	if(f && f->refs == 0 && f->is_deleted){
		delete_file(f);
	}
	return 0;
}

int
ufs_delete(const char *filename)
{
	file* opened_file = get_ufs_file(filename);
	if(!opened_file || opened_file->is_deleted) return 0;
	opened_file->is_deleted = true;
	rlist_del_entry(opened_file, in_file_list);

	if(opened_file->refs == 0) {
		delete_file(opened_file);
	}
	return 0;
}

#if NEED_RESIZE

static void
expand_file(file *f, size_t new_size){
	if(f->size >= new_size) return;

	size_t sz = f->size - (f->size % static_cast<size_t>(BLOCK_SIZE)) + static_cast<size_t>(BLOCK_SIZE);
	if(new_size <= sz){
		f->size = new_size;
		return;
	}
		
	while(sz < new_size){
		block *new_block = new block{};
		memset(new_block->memory, 0, BLOCK_SIZE);
		rlist_add_tail_entry(&f->blocks, new_block, in_block_list);
		sz += static_cast<size_t>(BLOCK_SIZE);
	}
	f->size = new_size;
}

static void
shrink_file(file *f, size_t new_size){
	size_t sz = f->size - (f->size % static_cast<size_t>(BLOCK_SIZE));
	if(sz <= new_size){
		f->size = new_size;
		return;
	}
	sz += static_cast<size_t>(BLOCK_SIZE);
	
	block *blk = rlist_last_entry(&f->blocks, struct block, in_block_list);
	
	while(sz > new_size){
		rlist *tmp = blk->in_block_list.prev;
		
		if (tmp == &f->blocks) {
			break;
		}

		rlist_del_entry(blk, in_block_list);
		delete blk;
		
		blk = rlist_entry(tmp, struct block, in_block_list);
		sz -= static_cast<size_t>(BLOCK_SIZE);
	}
	f->size = new_size;
}

int
ufs_resize(int fd, size_t new_size)
{
	filedesc* desc = get_filedesc(fd);
	if(!desc) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

	if (new_size > MAX_FILE_SIZE) {
		ufs_error_code = UFS_ERR_NO_MEM;
		return -1;
	}

#if NEED_OPEN_FLAGS
	if(!(desc->mode & UFS_READ_WRITE) && desc->mode != 0){
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	file *f = desc->atfile;
	if(f->size == new_size) return 0;

	if(f->size < new_size){
		expand_file(f, new_size);
		return 0;
	}
	
	shrink_file(f, new_size);
	return 0;
}

#endif

/*
 * The file_descriptors array is likely to leak even if
 * you resize it to zero or call clear(). This is because
 * the vector keeps memory reserved in case more elements
 * would be added.
 *
 * The recommended way of freeing the memory is to swap()
 * the vector with a temporary empty vector.
 */
void
ufs_destroy(void)
{
	for(int fd = 0; static_cast<size_t>(fd) < file_descriptors.size(); ++fd){
		ufs_close(fd);
	}
	std::vector<filedesc*> tmp;
	std::swap(file_descriptors, tmp);

	file *file_item, *tmp_file;
	rlist_foreach_entry_safe(file_item, &file_list, in_file_list, tmp_file) {
		file *f = rlist_entry(file_item, struct file, in_file_list);
		delete_blocks_in_file(f);
		rlist_del_entry(f, in_file_list);
		delete f;
	}
}
