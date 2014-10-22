#ifdef _MSC_VER
#  define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! [hello_world_jit implementation.]
#include <likely.h>

int main(int argc, char *argv[])
{
    if ((argc < 3) || (argc > 4)) {
        puts("Usage:\n\t"
                 "hello_world_jit <input_image> <function> [<output_image>]\n"
             "\n"
             "Example:\n\t"
                 "hello_world_jit data/misc/lenna.tiff 'a :-> (=> a (/ a (a.type 2)))' dark_lenna.png\n");
        return EXIT_FAILURE;
    }

    puts("Reading input image...");
    likely_const_mat input = likely_read(argv[1], likely_file_binary);
    likely_assert(input, "failed to read: %s", argv[1]);
    printf("Width: %u\nHeight: %u\n", input->columns, input->rows);

    puts("Parsing function...");
    likely_const_ast ast = likely_lex_and_parse(argv[2], likely_source_lisp);

    puts("Creating a compiler environment...");
    likely_const_env env = likely_new_env_jit();

    puts("Compiling source code...");
    likely_const_fun fun = likely_compile(ast->atoms[0], env, likely_matrix_void);
    likely_assert(fun->function, "failed to compile: %s", argv[2]);

    puts("Calling compiled function...");
    // We expect fun->function to be a function that takes one constant matrix as input
    // and returns a new matrix as output.
    likely_mat (*f)(likely_const_mat) = fun->function;
    likely_const_mat output = f(input);
    likely_assert(output, "failed to execute compiled function");

    if (argc >= 4) {
        puts("Writing output image...");
        likely_assert(likely_write(output, argv[3]), "failed to write: %s", argv[3]);
    }

    puts("Cleaning up...");
    likely_release_mat(output);
    likely_release_fun(fun);
    likely_release_env(env);
    likely_release_ast(ast);
    likely_release_mat(input);

    puts("Done!");
    likely_shutdown();

    return EXIT_SUCCESS;
}
//! [hello_world_jit implementation.]
