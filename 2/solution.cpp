#include "parser.h"

#include <assert.h>
#include <cstdio>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/wait.h>
#include <limits.h>


struct console_command{
	expr e;
	enum output_type out_type = OUTPUT_TYPE_STDOUT;
	/** Non-empty if the out type is FILE. */
	std::string out_file;
	bool is_background = false;
};

static void to_file(enum output_type out_type, const std::string& out_file){
	if(out_type != OUTPUT_TYPE_STDOUT){
		int file_fd{};
		if(out_type == OUTPUT_TYPE_FILE_NEW){
			file_fd = open(out_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		}else{
			file_fd = open(out_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
		}
    	dup2(file_fd, STDOUT_FILENO);
    	close(file_fd);
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

static int wait_for_pipeline(std::vector<int>& children_pids, bool is_background = false) {
    if(is_background){
		children_pids.clear();
		return 0;
	}

	int last_exit_code = 0;
    for (auto child_pid : children_pids) {
        int status = 0;
        if (waitpid(child_pid, &status, 0) > 0) {
            if (WIFEXITED(status) && child_pid == children_pids.back()) {
                last_exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status) && child_pid == children_pids.back()) {
                last_exit_code = 128 + WTERMSIG(status);
            }
        }
    }
	children_pids.clear();
    return last_exit_code;
}

static void
execute_change_dir()
{
	if (chdir("~") != 0) {
        exit(1);
    }
}

static void
execute_change_dir(const std::string& path)
{
	if (chdir(path.c_str()) != 0) {
        exit(1);
    }
}

static void
execute_change_dir(const command& cmd)
{
	assert(cmd.exe == "cd");
	assert(cmd.args.size() <= 1);
	if(cmd.args.empty()){
		execute_change_dir();
	} else {
		execute_change_dir(cmd.args.front());
	}
}

static void
execute_echo(const console_command& cmd)
{
	assert(cmd.e.cmd.has_value());
	assert(cmd.e.cmd->exe == "echo");

	if(cmd.e.cmd->args.empty()){
		return;
	}
	for (size_t i = 0; i < cmd.e.cmd->args.size(); ++i) {
		if(i > 0){
			printf(" ");
		}
	    printf("%s", cmd.e.cmd->args[i].c_str());
	}
	printf("\n");
	fflush(stdout);
}

static void
execute_exit(const command& cmd)
{
	assert(cmd.exe == "exit");
	if(cmd.args.empty()){
		exit(0);
	}

	if(cmd.args.size() > 1){
		printf("%s: too many arguments\n", cmd.exe.c_str());
		return;
	}

	char *endptr;
    long code = strtol(cmd.args[0].c_str(), &endptr, 10);
    if (cmd.args[0].c_str() == endptr || code > INT_MAX || code < INT_MIN){
		printf("%s: %s: numeric argument required\n", cmd.exe.c_str(), cmd.args[0].c_str());
		return;
	}

	exit(static_cast<int>(code));
}

static void
execute_cmd_in_forked_process(const console_command& cmd, std::vector<int>& children_pids, int in, int out, int current_pipe_read)
{
	auto pid = fork();
	if (pid < 0) {
    	exit(1);
	} 
	else if (pid == 0) {
		boundPipes(in, out);

		if (current_pipe_read != -1) {
			close(current_pipe_read);
		}

		if (out == -1) {
		    to_file(cmd.out_type, cmd.out_file);
		}
		
		if(cmd.e.cmd->exe == "echo"){
			execute_echo(cmd);
			_exit(0);
		} else if(cmd.e.cmd->exe == "exit"){
			execute_exit(*cmd.e.cmd);
		}
		
		unsigned num_of_args = cmd.e.cmd->args.size() + 2;
		char** c_args = new char*[num_of_args];
		c_args[0] = const_cast<char*>(cmd.e.cmd->exe.c_str());
		unsigned cnt = 1;
		for (const auto& arg : cmd.e.cmd->args) {
		    c_args[cnt] = const_cast<char*>(arg.c_str());
			++cnt;
		}
		c_args[num_of_args - 1] = nullptr;
		
		execvp(cmd.e.cmd->exe.c_str(), c_args);
    	
		_exit(EXIT_FAILURE); 
	}
	children_pids.push_back(pid);
}

static void
execute_single_cmd(const console_command& c, std::vector<int>& children_pids, int in, int out, int current_pipe_read, bool is_single_cmd = false)
{
	if(!c.e.cmd){
		return;
	}else if(c.e.cmd->exe == "cd"){
		execute_change_dir(*c.e.cmd);
	} else if(c.e.cmd->exe == "exit" and is_single_cmd){
		execute_exit(*c.e.cmd);
	} else {
		execute_cmd_in_forked_process(c, children_pids, in, out, current_pipe_read);
	}
}

static void
execute_piped_cmds(std::vector<console_command> piped_cmd, std::vector<int>& children_pids){
	if(piped_cmd.empty()) return;
	if(piped_cmd.size() == 1 and piped_cmd.size() == 1 and piped_cmd.front().e.type == EXPR_TYPE_COMMAND){
		execute_single_cmd(piped_cmd.front(), children_pids, -1, -1, -1, true);
		return;
	}

	int prev_pipe_read = -1;
	
	for(size_t i = 0; i < piped_cmd.size(); ++i){
		int fd[2] = {-1, -1};

        if (i < piped_cmd.size() - 1) {
            if (pipe(fd) == -1) {
                perror("pipe failed");
                exit(1);
            }
        }

        int in = prev_pipe_read;
        int out = (i < piped_cmd.size() - 1) ? fd[1] : -1;

        execute_single_cmd(piped_cmd[i], children_pids, in, out, fd[0]);

        if (prev_pipe_read != -1) {
            close(prev_pipe_read);
        }
		if (fd[1] != -1) {
            close(fd[1]);
        }
        prev_pipe_read = fd[0];
	}
}

static int
execute_command_line(const struct command_line *line)
{
	/* REPLACE THIS CODE WITH ACTUAL COMMAND EXECUTION */

	//assert(line != NULL);
	//printf("================================\n");
	//printf("Command line:\n");
	//printf("Is background: %d\n", (int)line->is_background);
	//printf("Output: ");
	//if (line->out_type == OUTPUT_TYPE_STDOUT) {
	//	printf("stdout\n");
	//} else if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
	//	printf("new file - \"%s\"\n", line->out_file.c_str());
	//} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
	//	printf("append file - \"%s\"\n", line->out_file.c_str());
	//} else {
	//	assert(false);
	//}
	//printf("Expressions:\n");
	//for (const expr &e : line->exprs) {
	//	if (e.type == EXPR_TYPE_COMMAND) {
	//		printf("\tCommand: %s", e.cmd->exe.c_str());
	//		for (const std::string& arg : e.cmd->args)
	//			printf(" %s", arg.c_str());
	//		printf("\n");
	//	} else if (e.type == EXPR_TYPE_PIPE) {
	//		printf("\tPIPE\n");
	//	} else if (e.type == EXPR_TYPE_AND) {
	//		printf("\tAND\n");
	//	} else if (e.type == EXPR_TYPE_OR) {
	//		printf("\tOR\n");
	//	} else {
	//		assert(false);
	//	}
	//}

	if(line == NULL){
		return 0;
	}

	std::vector<int> children_pids;

	std::vector<console_command> piped_cmd;
	
	bool is_hop = false;
	int last_exit_code = 0;

	for (const expr &e : line->exprs) {
		if (e.type == EXPR_TYPE_COMMAND) {
			if(is_hop) continue;
			piped_cmd.push_back({console_command{e, line->out_type, line->out_file, line->is_background}});
		} else if (e.type == EXPR_TYPE_PIPE) {
			continue;
		} else if (e.type == EXPR_TYPE_AND) {
			is_hop = false;
			execute_piped_cmds(piped_cmd, children_pids);
			last_exit_code = wait_for_pipeline(children_pids, line->is_background);
			if(last_exit_code != 0) is_hop = true;
			piped_cmd.clear();
		} else if (e.type == EXPR_TYPE_OR) {
			is_hop = false;
			execute_piped_cmds(piped_cmd, children_pids);
			last_exit_code = wait_for_pipeline(children_pids, line->is_background);
			if(last_exit_code == 0) is_hop = true;
			piped_cmd.clear();
		} else {
			assert(false);
		}
	}

	execute_piped_cmds(piped_cmd, children_pids);

	if (!children_pids.empty()) {
		last_exit_code = wait_for_pipeline(children_pids, line->is_background);
	}
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
			while (waitpid(-1, nullptr, WNOHANG) > 0);
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			last_exit_code = execute_command_line(line);
			delete line;
		}
	}
	parser_delete(p);
	return last_exit_code;
}
