/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include "interval.h"
#include "atomic.h"
#include "internal.h"

FILE *dump;

typedef struct board_t board_t;
struct board_t {
	int *board;
	int n;
};

board_t *board_create(int n) {
	board_t *board = (board_t*)malloc(sizeof(board_t));
	board->n = n;
	board->board = (int*)malloc(n*sizeof(int));
	for(int i = 0; i < n; i++)
		board->board[i] = -1;
	return board;
}

void board_free(board_t *board) {
	free(board->board);
	free(board);
}

bool is_valid_move(board_t *board, int row, int col) {
	for(int i = 0; i < row; i++) {
		int bi = board->board[i];
		// check for same col or diagonal conflict
		if(bi == col || abs(bi - col) == row - i)
			return false;
	}
	return true;
}

void put_queen_to(board_t *board, int row, int col) {
	board->board[row] = col;
}

void remove_queen_from(board_t *board, int row, int col) {
	board->board[row] = -1;
}

char *board_to_string(board_t *board) {
	char *buffer = (char*) malloc(sizeof(char) * (board->n+1) * board->n);
	
	char *p = buffer;
	for(int i = 0; i < board->n; i++) {
		for(int j = 0; j < board->n; j++) {
			if(board->board[i] == j)
				*p++ = 'O';
			else
				*p++ = 'X';
		}
		*p++ = '\n';
	}
	
	return buffer;
}

typedef struct position_list_t position_list_t;
struct position_list_t {
	int row;
	int col;
	position_list_t *previous;
};

position_list_t *position_list_create(int row, int col, position_list_t *prev) {
	position_list_t *node = (position_list_t*)malloc(sizeof(position_list_t));
	node->row = row;
	node->col = col;
	node->previous = prev;
	debugf("%p = <%d,%d,%p> -- created", node, row, col, prev);
	return node;
}

void position_list_free(position_list_t *pos) {
	debugf("%p -- freed", pos);
	free(pos);
}

void position_list_dump(position_list_t *pos) {
	if(dump) {
		fprintf(dump, "%20p position:", pthread_self());
		for(position_list_t *cur = pos; cur != NULL; cur = cur->previous) {
			fprintf(dump, " <%d,%d>", cur->row, cur->col);
		}
		fprintf(dump, "\n");
	}
}

board_t *position_list_to_board(position_list_t *position, int problem_size) {
	board_t *result = board_create(problem_size);
		
	int i = position->row;
	for(position_list_t *current = position; current != NULL; current = current->previous)
		result->board[i--] = current->col;
	
	return result;
}

// returns a newly allocated null-terminated array of size problem_size + 1
position_list_t **generate_valid_modes(position_list_t *position, int problem_size) {
	int *invalid_positions = (int*)calloc(problem_size, sizeof(int));
	int next_row = position->row + 1;
	
	for(position_list_t *current = position; current != NULL; current = current->previous) {
		invalid_positions[current->col] = 1;
		
		int d_row = next_row - current->row;
		if(current->col + d_row < problem_size)
			invalid_positions[current->col + d_row] = 1;
		if(current->col - d_row >= 0)
			invalid_positions[current->col - d_row] = 1;
	}
	
	position_list_t **valid_moves = (position_list_t**)calloc(sizeof(position_list_t), problem_size+1);
	for(int i = 0, l = 0; i < problem_size; i++) {
		if(!invalid_positions[i])
			valid_moves[l++] = position_list_create(next_row, i, position);
	}
	
	free(invalid_positions);
	return valid_moves;
}

void solve_sequentially(board_t *board,
						int row, 
						int *solution_count) 
{
	assert(row <= board->n);
	if(row < board->n) {
		for(int col = 0; col < board->n; col++)
			if(is_valid_move(board, row, col)) {
				put_queen_to(board, row, col);
				solve_sequentially(board, row + 1, solution_count);
				remove_queen_from(board, row, col);
			}
	} else {
		atomic_add(solution_count, 1);
	} 
}

void solve_one_level(position_list_t *start_position, // will be freed 
					 int problem_size, 
					 int cutoff_level, 
					 int *solution_count)
{
	subinterval(^(point_t *sub_end) {
		int current_row = start_position->row + 1;
		debugf("solve_one_level(%p=<%d,%d>, %d, %d)", start_position, start_position->row, start_position->col,
			   problem_size, cutoff_level);
		assert(current_row <= problem_size);
		if(current_row < problem_size) {
			if(current_row == cutoff_level) {
				board_t *board = position_list_to_board(start_position, problem_size);
				position_list_dump(start_position);
				solve_sequentially(board, current_row, solution_count);
				board_free(board);
			} else {
				position_list_t **valid_moves = generate_valid_modes(start_position, problem_size);
				for(int i = 0; valid_moves[i]; i++) {
					position_list_t *move = valid_moves[i];
					debugf("%p led to move %p=<%d,%d>", start_position, move, move->row, move->col);
					
					interval(sub_end, ^(point_t *_) {
						solve_one_level(move, problem_size, cutoff_level, solution_count);					
					});
				}
				free(valid_moves);
			}
		} else {
			// we've reached the last possible level, which means we have a solution
			atomic_add(solution_count, 1);
		}
	});
	
	position_list_free(start_position);
}

void solve_in_parallel(int problem_size, int cutoff_level, int *solution_count)
{
	assert(cutoff_level > 0);
	if(cutoff_level == 0) {
		board_t *board = board_create(problem_size);
		solve_sequentially(board, 0, solution_count);
		board_free(board);
	} else {
		for(int i = 0; i < problem_size; i++) {
			position_list_t *pos = position_list_create(0, i, NULL);
			
			interval(NULL, ^(point_t *_) {
				solve_one_level(pos, problem_size, cutoff_level, solution_count);
			});
		}
	}
}

int main(int argc, char *argv[]) {
	if(argc != 3) {
		fprintf(stderr, "Usage: nqueens problem_size cutoff_level");
		return 1;
	}
	
//	dump = fopen("nqueens_dump.txt", "w");
	
	const int problem_size = atoi(argv[1]);
	const int cutoff_level = atoi(argv[2]);
	
	int seq_solution_count = 0;
	clock_t seq_clock0 = clock();
	board_t *board = board_create(problem_size);
//	solve_sequentially(board, 0, &seq_solution_count);
	board_free(board);
	clock_t seq_clock1 = clock();
	double seq_clockd = seq_clock1 - seq_clock0;
		
	printf("Seq. Solution Count: %d\n", seq_solution_count);
	printf("Seq. Processor Time: %.3f seconds\n", seq_clockd / CLOCKS_PER_SEC);
	fflush(stdout);
	
	int *par_solution_count = (int*)malloc(sizeof(int));
	*par_solution_count = 0;
	clock_t par_clock0 = clock();
	root_interval(^(point_t *end) {
		solve_in_parallel(problem_size, cutoff_level, par_solution_count);
	});
	clock_t par_clock1 = clock();
	double par_clockd = par_clock1 - par_clock0;
	
	printf("Par. Solution Count: %d\n", *par_solution_count);
	printf("Par. Processor Time: %.3f seconds\n", par_clockd / CLOCKS_PER_SEC);
	free(par_solution_count);

	printf("Par/Seq Ratio: %.3f", par_clockd / seq_clockd);
	return 0;
}
