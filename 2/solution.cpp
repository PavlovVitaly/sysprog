#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/wait.h>
#include <limits.h>


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
execute_echo(const command& cmd)
{
	assert(cmd.exe == "echo");
	if(cmd.args.empty()){
		return;
	}
	for (size_t i = 0; i < cmd.args.size(); ++i) {
		if(i > 0){
			printf(" ");
		}
	    printf("%s", cmd.args[i].c_str());
	}
	printf("\n");

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
execute_cmd_in_forked_process(const command& cmd, std::vector<int>& children_pids)
{
	auto pid = fork();
	if (pid < 0) {
    	exit(1);
	} 
	else if (pid == 0) {
		unsigned num_of_args = cmd.args.size() + 2;
		char* c_args[num_of_args];
		c_args[0] = const_cast<char*>(cmd.exe.c_str());
		unsigned cnt = 1;
		for (const auto& arg : cmd.args) {
		    c_args[cnt] = const_cast<char*>(arg.c_str());
			++cnt;
		}
		c_args[num_of_args - 1] = nullptr;
		execvp(cmd.exe.c_str(), c_args);
    	_exit(EXIT_FAILURE); 
	}
	children_pids.push_back(pid);
}

static void
execute_single_cmd(const expr& e, std::vector<int>& children_pids)
{
	if(!e.cmd){
		return;
	}else if(e.cmd->exe == "echo"){
		execute_echo(*e.cmd);
	}else if(e.cmd->exe == "cd"){
		execute_change_dir(*e.cmd);
	} else if(e.cmd->exe == "exit"){
		execute_exit(*e.cmd);
	} else {
		execute_cmd_in_forked_process(*e.cmd, children_pids);
	}
}

static void
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
		return;
	}

	std::vector<int> children_pids;

	for (const expr &e : line->exprs) {
		if (e.type == EXPR_TYPE_COMMAND) {
			execute_single_cmd(e, children_pids);
		} else if (e.type == EXPR_TYPE_PIPE) {
			assert(false);
		} else if (e.type == EXPR_TYPE_AND) {
			assert(false);
		} else if (e.type == EXPR_TYPE_OR) {
			assert(false);
		} else {
			assert(false);
		}
	}

	for(auto child_pid : children_pids){
		int status;
		waitpid(child_pid, &status, 0);	
	}
}

int
main(void)
{
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			execute_command_line(line);
			delete line;
		}
	}
	parser_delete(p);
	return 0;
}
