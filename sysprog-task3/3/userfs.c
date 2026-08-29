#include "userfs.h"
#include <stddef.h>
#include <stdlib.h> 
#include <string.h>
#include <stdio.h>
#include <assert.h>

enum {
	BLOCK_SIZE = 512,
	MAX_FILE_SIZE = 1024 * 1024 * 100,
};

/** Global error code. Set from any function on any error. */
static enum ufs_error_code ufs_error_code = UFS_ERR_NO_ERR;

struct block {
	/** Block memory. */
	char *memory;
	/** How many bytes are occupied. */
	int occupied;
	/** Next block in the file. */
	struct block *next;
	/** Previous block in the file. */
	struct block *prev;

	/* PUT HERE OTHER MEMBERS */
};

struct file {
	/** Double-linked list of file blocks. */
	struct block *block_list;
	/**
	 * Last block in the list above for fast access to the end
	 * of file.
	 */
	struct block *last_block;
	/** How many file descriptors are opened on the file. */
	int refs;
	/** File name. */
	char *name;
	/** Files are stored in a double-linked list. */
	struct file *next;
	struct file *prev;

	/* PUT HERE OTHER MEMBERS */
	size_t size;
	bool is_deleted;
};

/** List of all files. */
static struct file *file_list = NULL;

struct filedesc {
	struct file *file;

	/* PUT HERE OTHER MEMBERS */
	struct block *block_ptr;
	size_t block_num;
	size_t offset;
	int mode;
};

/**
 * An array of file descriptors. When a file descriptor is
 * created, its pointer drops here. When a file descriptor is
 * closed, its place in this array is set to NULL and can be
 * taken by next ufs_open() call.
 */
static struct filedesc **file_descriptors = NULL;
static int file_descriptor_count = 0;
static int file_descriptor_capacity = 0;

static int
resize_file_descriptors(int size){
	if(size < 0) return -1;
	
	struct filedesc **new_vector = realloc(file_descriptors, size * sizeof(struct filedesc*));
	if(!new_vector) return -1;
	
	if(size > file_descriptor_capacity) {
		memset(new_vector + file_descriptor_capacity, 0, (size - file_descriptor_capacity) * sizeof(struct filedesc*));
	}
	
	file_descriptors = new_vector;
	file_descriptor_capacity = size;
	return 0;
}

static int
push_back_file_descriptors(struct filedesc *item){
	if(file_descriptor_capacity <= 0){
		file_descriptors = calloc(1, sizeof(struct filedesc*));
		if(!file_descriptors) return -1;
		file_descriptor_capacity = 1;
	}
	
	if(file_descriptor_count >= file_descriptor_capacity){
		if(resize_file_descriptors(file_descriptor_capacity * 2) < 0) return -1;
	}
	
	file_descriptors[file_descriptor_count++] = item;
	return 0;
}

enum ufs_error_code
ufs_errno()
{
	return ufs_error_code;
}

static struct file
*create_ufs_file(const char *filename){
	struct file *new_file = malloc(sizeof(struct file));
	new_file->name = strdup(filename);
	new_file->block_list = NULL;
	new_file->last_block = NULL;
	new_file->is_deleted = false;
	new_file->refs = 0;
	new_file->size = 0;
	new_file->next = NULL;
	new_file->prev = NULL;
	if(!file_list){
		file_list = new_file;
		return new_file;
	}

	struct file *tmp = file_list;
	while (tmp->next) {
		tmp = tmp->next;
	}
	tmp->next = new_file;
	new_file->prev = tmp;
	return new_file;
}

static struct file
*get_ufs_file(const char *filename){
	if(!file_list) return NULL;
	struct file* f = file_list;
	while(f){
		if(strcmp(f->name, filename) == 0 && !f->is_deleted) return f;
		f = f->next;
	}
	return NULL;
}

static int
find_free_filedescriptor(){
	for(int fd = 0; fd < file_descriptor_count; ++fd){
		if(file_descriptors[fd] == NULL){
			return fd;
		}
	}
	return -1;
}

static struct filedesc
*create_ufs_filedesc(struct file *opened_file, int flags){
	struct filedesc *new_filedesc = malloc(sizeof(struct filedesc));
	new_filedesc->mode = flags;
	struct block *blk = opened_file->block_list;
	new_filedesc->block_ptr = blk;
	new_filedesc->offset = 0;
	new_filedesc->block_num = 0;
	new_filedesc->file = opened_file;
	opened_file->refs++;
	return new_filedesc;
}

static void
actualize_file_descriptor(struct filedesc *desc){
	size_t sz = desc->block_num * BLOCK_SIZE + desc->offset;
	if(sz > desc->file->size){
		desc->offset = desc->file->size % BLOCK_SIZE;
		desc->block_num = desc->file->size / BLOCK_SIZE;
	}

	if(desc->file->block_list){
		size_t cnt = 0;
		struct block *blk = desc->file->block_list;
		while(blk){
			desc->block_ptr = blk;
			if(cnt == desc->block_num){
				break;
			}
			++cnt;
			blk = blk->next;
		}
	} else {
		desc->block_ptr = NULL;
		desc->block_num = 0;
		desc->offset = 0;
	}
}

static struct filedesc 
*get_filedesc(int fd){
	if(fd < 0 || fd >= file_descriptor_count) {
		return NULL;
	}
	struct filedesc *desc = file_descriptors[fd];
	if(!desc) return NULL;
	
	actualize_file_descriptor(desc);
	return desc;
}

int
ufs_open(const char *filename, int flags)
{
	struct file *opened_file = get_ufs_file(filename);
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

	const int fd = find_free_filedescriptor();

	struct filedesc *desc = create_ufs_filedesc(opened_file, flags);
	if(fd < 0){
		push_back_file_descriptors(desc);
		return file_descriptor_count - 1;
	}
	
	file_descriptors[fd] = desc;
	return fd;
}

static void
create_init_block(struct filedesc *fd){
	struct block *new_block = malloc(sizeof(struct block));
	new_block->next = NULL;
	new_block->prev = NULL;
	new_block->occupied = 0;
	new_block->memory = calloc(BLOCK_SIZE, sizeof(char));
	fd->file->block_list = new_block;
	fd->file->last_block = new_block;
	fd->block_ptr = new_block;
	fd->block_num = 0;
	fd->offset = 0;
}

static void
add_new_empty_block(struct filedesc *fd){
	struct block *new_block = malloc(sizeof(struct block));
	new_block->next = NULL;
	new_block->prev = fd->file->last_block;
	fd->file->last_block->next = new_block;
	fd->file->last_block = new_block;
	new_block->occupied = 0;
	new_block->memory = calloc(BLOCK_SIZE, sizeof(char));
	fd->block_ptr = new_block;
	fd->block_num++;
	fd->offset = 0;
}

static void
update_file_size(struct filedesc *fd){
	size_t new_file_size = BLOCK_SIZE * fd->block_num + fd->offset;
	if(new_file_size > fd->file->size){
		fd->file->size = new_file_size;
	}
}

static ssize_t
ufs_write_data_to_current_block(struct filedesc *desc, const char *buf, size_t size){
	if(size > BLOCK_SIZE - desc->offset) return -1;
	memcpy((desc->block_ptr->memory + desc->offset), buf, size);
	desc->offset += size;
	update_file_size(desc);
	return size;
}

static void
force_shift_to_next_block(struct filedesc *desc){
	if(!desc->block_ptr->next){
		add_new_empty_block(desc);
		return;
	}

	struct block* blk = desc->block_ptr->next;	
	desc->block_ptr = blk;
	desc->block_num++;
	desc->offset = 0;
}

ssize_t
ufs_write(int fd, const char *buf, size_t size)
{
	struct filedesc* desc = get_filedesc(fd);
	if(!desc) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

#if NEED_OPEN_FLAGS
	if(!(desc->mode & UFS_WRITE_ONLY) && !(desc->mode & UFS_READ_WRITE) && desc->mode != 0){
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	if(desc->file->size == 0 && !desc->block_ptr && size != 0){
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
	size_t new_size = 0;

	while(total_size < size){
		if (desc->offset == BLOCK_SIZE) force_shift_to_next_block(desc);
		size_t diff = size - total_size;
		new_size = BLOCK_SIZE - desc->offset >= diff ? diff : BLOCK_SIZE - desc->offset;
		total_size += ufs_write_data_to_current_block(desc, buf + total_size, new_size);	
	}

	update_file_size(desc);
	return total_size;
}

static ssize_t
ufs_read_data_to_current_block(struct filedesc *desc, char *buf, size_t size){
	if(BLOCK_SIZE - desc->offset < size) return -1;
	if(size == 0) return 0;

	memcpy(buf, (desc->block_ptr->memory + desc->offset), size);
	desc->offset += size;
	return size;
}

static bool
soft_shift_to_next_block(struct filedesc *desc){
	if (desc->offset != BLOCK_SIZE) return true;
	if (!desc->block_ptr->next) return false;
	desc->block_ptr = desc->block_ptr->next;
	desc->block_num++;
	desc->offset = 0;
	return true;
}

ssize_t
ufs_read(int fd, char *buf, size_t size)
{
	struct filedesc* desc = get_filedesc(fd);
	if(!desc) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}

#if NEED_OPEN_FLAGS
	if(!(desc->mode & UFS_READ_ONLY) && !(desc->mode & UFS_READ_WRITE) && desc->mode != 0){
		ufs_error_code = UFS_ERR_NO_PERMISSION;
		return -1;
	}
#endif

	if(!desc->block_ptr) return 0;

	size_t tmp = desc->file->size - BLOCK_SIZE * desc->block_num - desc->offset;
	size_t rsize = tmp < size ? tmp : size;
	if(BLOCK_SIZE - desc->offset >= rsize){
		return ufs_read_data_to_current_block(desc, buf, rsize);
	}

	if(rsize == 0) return 0;
	size_t total_size = ufs_read_data_to_current_block(desc, buf, BLOCK_SIZE - desc->offset);

	while(total_size < rsize){
		if (desc->offset == BLOCK_SIZE) {
			if (!soft_shift_to_next_block(desc)) break;
		}
		size_t diff = rsize - total_size;
		size_t blk_sz = BLOCK_SIZE - desc->offset >= diff ? diff : BLOCK_SIZE - desc->offset;
		total_size += ufs_read_data_to_current_block(desc, buf + total_size, blk_sz);
	}

	return total_size;
}

static void
delete_blocks_in_file(struct file *f){
	struct block *blk = f->block_list;
	while(blk){
		struct block *tmp = blk->next;
		if (blk->memory) {
            free(blk->memory); // ИСПРАВЛЕНИЕ: сначала чистим буфер
        }
		free(blk);
		blk = tmp;
	}
}

static void
delete_file(struct file *f){
	delete_blocks_in_file(f);
	if (f->name) {
        free(f->name);
    }
	if(f->prev){
        f->prev->next = f->next;
    } else {
        assert(file_list == f);
        file_list = f->next;
    }
    if (f->next) {
        f->next->prev = f->prev;
    }
    free(f);
}

int
ufs_close(int fd)
{
	struct filedesc *desc = get_filedesc(fd);
	if(!desc) {
		ufs_error_code = UFS_ERR_NO_FILE;
		return -1;
	}
	struct file *f = desc->file;
	f->refs--;
	
	free(desc);
	file_descriptors[fd] = NULL;
	
	if(f && f->refs == 0 && f->is_deleted){
		delete_file(f);
	}
	return 0;
}

int
ufs_delete(const char *filename)
{
	struct file *opened_file = get_ufs_file(filename);
	if(!opened_file || opened_file->is_deleted) return 0;
	opened_file->is_deleted = true;
	if(opened_file->refs == 0) {
		delete_file(opened_file);
	}
	return 0;
}

#if NEED_RESIZE

static void
expand_file(struct file *f, size_t new_size){
	if(f->size >= new_size) return;

	if (!f->block_list) {
		struct block *new_block = calloc(1, sizeof(struct block));
		new_block->memory = calloc(BLOCK_SIZE, sizeof(char));
		f->block_list = new_block;
		f->last_block = new_block;
	}

	size_t sz = f->size - (f->size % BLOCK_SIZE) + BLOCK_SIZE;
	if(new_size <= sz){
		f->size = new_size;
		return;
	}
		
	while(sz < new_size){
		struct block *new_block = malloc(sizeof(struct block));
		new_block->next = NULL;
		new_block->prev = f->last_block;
		new_block->occupied = 0;
		new_block->memory = calloc(BLOCK_SIZE, sizeof(char));
		f->last_block->next = new_block;
		f->last_block = new_block;
		sz += BLOCK_SIZE;
	}
	f->size = new_size;
}


static void
shrink_file(struct file *f, size_t new_size){
	size_t sz = f->size - (f->size % BLOCK_SIZE);
	if(sz <= new_size){
		f->size = new_size;
		return;
	}
	sz += BLOCK_SIZE;
	
	struct block *blk = f->last_block;
	
	while(sz > new_size){
		struct block *tmp = blk->prev;
		
		if (!tmp) {
			break;
		}

		if (blk->memory) {
            free(blk->memory);
        }
		free(blk);
		blk = tmp;
		blk->next = NULL;
		sz -= BLOCK_SIZE;
	}
	f->last_block = blk;
	if (!blk) {
		f->block_list = NULL;
	}
	f->size = new_size;
}

int
ufs_resize(int fd, size_t new_size)
{
	struct filedesc* desc = get_filedesc(fd);
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

	struct file *f = desc->file;
	if(f->size == new_size) return 0;

	if(f->size < new_size){
		expand_file(f, new_size);
		return 0;
	}
	
	shrink_file(f, new_size);
	return 0;
}

#endif

void
ufs_destroy(void)
{
	for(int fd = 0; fd < file_descriptor_count; ++fd){
		ufs_close(fd);
	}
	free(file_descriptors);

	while(file_list){
		struct file *file_item = file_list;
		file_list = file_list->next;
		delete_blocks_in_file(file_item);
		free(file_item);
	}
}
