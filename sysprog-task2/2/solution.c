#include "parser.h"

#include <assert.h>
//#include <cstdint>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>

struct pid_vector {
	pid_t *data;
	size_t size;
	size_t capacity;
};
/*

static void
pid_vector_pop_first_many(struct pid_vector *vector, int *data, size_t count)
{
	assert(count <= vector->size);
	memcpy(data, vector->data, sizeof(data[0]) * count);
	vector->size -= count;
	memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
}

static unsigned
pid_vector_pop_first(struct pid_vector *vector)
{
	int data = 0;
	pid_vector_pop_first_many(vector, &data, 1);
	return data;
}
*/

static void
pid_vector_append_many(struct pid_vector *vector,
	const int *data, size_t count)
{
	if (vector->size + count > vector->capacity) {
		if (vector->capacity == 0)
			vector->capacity = 4;
		else
			vector->capacity *= 2;
		if (vector->capacity < vector->size + count)
			vector->capacity = vector->size + count;
		vector->data = realloc(vector->data,
			sizeof(vector->data[0]) * vector->capacity);
	}
	memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
	vector->size += count;
}

static void
pid_vector_append(struct pid_vector *vector, int data)
{
	pid_vector_append_many(vector, &data, 1);
}

static void
pid_vector_clear(struct pid_vector *vector){
	vector->size = 0;
}

static void
pid_vector_reset(struct pid_vector *vector){
	free(vector->data);
	vector->size = 0;
}

struct command_property {
	enum output_type out_type;
	/** Valid if the out type is FILE. */
	char *out_file;
	bool is_background;
};

struct command_list {
	struct expr *head;
	struct expr *tail;
	struct command_property property;
};

static void copy_expr(struct expr* des, const struct expr* src){
	des->type = src->type;
	des->cmd.args = malloc(src->cmd.arg_count * sizeof(char*));
	for(size_t i = 0; i < src->cmd.arg_count; ++i){
		des->cmd.args[i] = strdup(src->cmd.args[i]);
	}
	des->cmd.args[src->cmd.arg_count] = NULL; 
	des->cmd.exe = strdup(src->cmd.exe);
	des->cmd.arg_count = src->cmd.arg_count;
	des->cmd.arg_capacity = src->cmd.arg_capacity;
	des->next = NULL;
}

static void
push_back(struct command_list* line, const struct expr* e){
	struct expr* new_e = malloc(sizeof(struct expr));
	copy_expr(new_e, e);
	new_e->next = NULL;
	if(!line->head){
		line->head = new_e;
		line->tail = line->head;
		return;	
	}
	line->tail->next = new_e;
	line->tail = line->tail->next;
}

static void
clear_list(struct command_list* line){
	if(line->head == line->tail){
		free(line->head->cmd.exe);
		if(line->head->cmd.args){
			for(size_t i = 0; i < line->head->cmd.arg_count; ++i){
				free(line->head->cmd.args[i]);
			}
			free(line->head->cmd.args);
		}
		free(line->head);
		return;
	}
	
	while(line->head){
		struct expr* next = line->head->next;
		free(line->head->cmd.exe);
		if(line->head->cmd.args){
			for(size_t i = 0; i < line->head->cmd.arg_count; ++i){
				free(line->head->cmd.args[i]);
			}
			free(line->head->cmd.args);
		}
		free(line->head);
		line->head = next;
	}
}

static bool
is_empty_list(struct command_list* line){
	return line->head == NULL;
}

static bool
is_single_expr(struct command_list* line){
	return line->head && !line->head->next;
}

static void close_descriptor(int desc){
	if (desc != -1) {
        close(desc);
    }
}

static void boundPipes(int in, int out){
	if(in != -1){
		dup2(in, STDIN_FILENO);
    	close(in);
	}

	if(out != -1){
		dup2(out, STDOUT_FILENO);
    	close(out);
	}
}

static void to_file(enum output_type out_type, const char* out_file){
	if(out_type != OUTPUT_TYPE_STDOUT){
		int file_fd;
		if(out_type == OUTPUT_TYPE_FILE_NEW){
			file_fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		}else{
			file_fd = open(out_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
		}
    	dup2(file_fd, STDOUT_FILENO);
    	close(file_fd);
	}
}

static void
execute_home_change_dir()
{
	if (chdir("~") != 0) {
        exit(1);
    }
}

static void
change_dir(const char* path)
{
	if (chdir(path) != 0) {
        exit(1);
    }
}

static void
execute_change_dir(const struct command* cmd)
{
	assert(strcmp(cmd->exe, "cd") == 0);
	if(cmd->arg_count == 0){
		execute_home_change_dir();
	} else {
		change_dir(cmd->args[0]);
	}
}

static void
execute_exit(const struct command* cmd)
{
	assert(strcmp(cmd->exe, "exit") == 0);
	if(cmd->arg_count == 0){
		exit(0);
	}

	if(cmd->arg_count > 1){
		printf("%s: too many arguments\n", cmd->exe);
		return;
	}

	char *endptr;
    long code = strtol(cmd->args[0], &endptr, 10);
    if (cmd->args[0] == endptr || code > INT_MAX || code < INT_MIN){
		printf("%s: %s: numeric argument required\n", cmd->exe, cmd->args[0]);
		return;
	}

	exit((int)code);
}

static void
execute_echo(const struct command* cmd)
{
	assert(cmd);
	assert(strcmp(cmd->exe, "echo") == 0);

	if(cmd->arg_count == 0){
		return;
	}
	for (size_t i = 0; i < cmd->arg_count; ++i) {
		if(i > 0){
			printf(" ");
		}
	    printf("%s", cmd->args[i]);
	}
	printf("\n");
	fflush(stdout);
}

static int
wait_for_pipeline(struct pid_vector* children_pids, bool is_background) {
    if(is_background){
		pid_vector_clear(children_pids);
		return 0;
	}

	int last_exit_code = 0;
    for (size_t i = 0; i < children_pids->size; ++i) {
		int child_pid = children_pids->data[i];
        int status = 0;
        if (waitpid(child_pid, &status, 0) > 0) {
            if (WIFEXITED(status) && i == children_pids->size - 1) {
                last_exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status) && i == children_pids->size - 1) {
                last_exit_code = 128 + WTERMSIG(status);
            }
        }
    }
	pid_vector_clear(children_pids);
    return last_exit_code;
}

static void
execute_forked_cmd(const struct expr* e
				, const struct command_property* property
				, int in
				, int out
				, int current_pipe_read){
	boundPipes(in, out);

	if (current_pipe_read != -1) {
		close(current_pipe_read);
	}

	if (out == -1) {
	    to_file(property->out_type, property->out_file);
	}
	
	if(strcmp(e->cmd.exe, "echo") == 0){
		execute_echo(&e->cmd);
		_exit(0);
	} else if(strcmp(e->cmd.exe, "exit") == 0){
		execute_exit(&e->cmd);
	}

	size_t num_of_args = e->cmd.arg_count + 2;
	char** new_args = malloc(num_of_args * sizeof(char*));
	new_args[0] = strdup(e->cmd.exe);
	for(size_t i = 0; i < e->cmd.arg_count; ++i){
		new_args[i + 1] = strdup(e->cmd.args[i]);
	}
	new_args[num_of_args - 1] = NULL;

	execvp(e->cmd.exe, new_args);
 	
	_exit(EXIT_FAILURE);
}

static void
execute_cmd_in_forked_process(const struct expr* cmd
							, const struct command_property* property
							, struct pid_vector* children_pids
							, int in
							, int out
							, int current_pipe_read)
{
	pid_t pid = fork();
	if (pid < 0) {
    	exit(1);
	} 
	else if (pid == 0) {
		execute_forked_cmd(cmd, property, in, out, current_pipe_read);
	}
	pid_vector_append(children_pids, pid);
}

static void
execute_single_cmd(const struct expr* c
				, const struct command_property* property
				, struct pid_vector* children_pids
				, int in
				, int out
				, int current_pipe_read
				, bool is_single_cmd)
{
	if(!c){
		return;
	}else if(strcmp(c->cmd.exe, "cd") == 0){
		execute_change_dir(&c->cmd);
	} else if(strcmp(c->cmd.exe, "exit") == 0 && is_single_cmd){
		execute_exit(&c->cmd);
	} else {
		execute_cmd_in_forked_process(c, property, children_pids, in, out, current_pipe_read);
	}
}

static void
execute_piped_cmds(struct command_list* piped_cmd, struct pid_vector* children_pids){
	if(is_empty_list(piped_cmd)) return;
	if(is_single_expr(piped_cmd) && piped_cmd->head->type == EXPR_TYPE_COMMAND){
		execute_single_cmd(piped_cmd->head
				, &piped_cmd->property
				, children_pids
				, -1
				, -1
				, -1
				, true);
		return;
	}

	int prev_pipe_read = -1;
	
	struct expr* current = piped_cmd->head;
	while(current){
		int fd[2] = {-1, -1};

        if (current->next && pipe(fd) == -1) {
            perror("pipe failed");
            exit(1);
        }

        int in = prev_pipe_read;
        int out = (current->next) ? fd[1] : -1;

        execute_single_cmd(current
			, &piped_cmd->property
			, children_pids
			, in
			, out
			, fd[0]
			, false);

		close_descriptor(prev_pipe_read);
		close_descriptor(fd[1]);
        prev_pipe_read = fd[0];
		current = current->next;
	}
}

static int
execute_command_line(const struct command_line *line)
{
	if(line == NULL){
		return 0;
	}

	struct pid_vector children_pids = {0};

	struct command_list piped_cmd = {0};
	piped_cmd.property.is_background = line->is_background;
	piped_cmd.property.out_file = line->out_file;
	piped_cmd.property.out_type = line->out_type;
	
	bool is_hop = false;
	int last_exit_code = 0;

	const struct expr *e = line->head;
	while (e != NULL) {
		if (e->type == EXPR_TYPE_COMMAND) {
			if(is_hop) {
				e = e->next;
				continue;
			}
			push_back(&piped_cmd, e);
		} else if (e->type == EXPR_TYPE_PIPE) {
			e = e->next;
			continue;
		} else if (e->type == EXPR_TYPE_AND || e->type == EXPR_TYPE_OR) {
			execute_piped_cmds(&piped_cmd, &children_pids);
			last_exit_code = wait_for_pipeline(&children_pids, line->is_background);
			is_hop = (e->type == EXPR_TYPE_AND && last_exit_code != 0)
				|| (e->type == EXPR_TYPE_OR && last_exit_code == 0);
			clear_list(&piped_cmd);
		} else {
			assert(false);
		}
		e = e->next;
	}

	execute_piped_cmds(&piped_cmd, &children_pids);

	if (children_pids.size != 0) {
		last_exit_code = wait_for_pipeline(&children_pids, line->is_background);
	}

	clear_list(&piped_cmd);
	pid_vector_reset(&children_pids);

	return last_exit_code;
}

int
main(void)
{
	const size_t buf_size = 512 * 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	int last_exit_code = 0;
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			while (waitpid(-1, NULL, WNOHANG) > 0);
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			last_exit_code = execute_command_line(line);
			command_line_delete(line);
		}
	}
	parser_delete(p);
	return last_exit_code;
}
