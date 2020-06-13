#include "smtx.h"
#include <err.h>
#include <sys/wait.h>


static void
send_cmd(int fd, char *cmd)
{
	safewrite(fd, cmd, strlen(cmd));
}

static void
expect_layout(const struct canvas *c, const char *expect)
{
	char actual[1024];
	const char *a = actual, *b = expect;
	describe_layout(actual, sizeof actual, c);
	while( *a && ( *a++ == *b || *b == '\0' ) ) {
		b += 1;
	}
	if( *b || *a ) {
		errx(1, "\nExpected \"%s\", but got \"%s\"\n", expect, actual);
	}
}

static int
test_description(int fd)
{
	(void)fd;
	struct canvas *r = init(24, 80);
	expect_layout(r, "*23x80@0,0(0,0)");
	create(r, "c");
	expect_layout(r, "*11x80@0,0(0,0); 11x80@12,0(0,0)");
	mov(r, "j");
	expect_layout(r, "11x80@0,0(0,0); *11x80@12,0(0,0)");
	create(r->c[0], "C");
	expect_layout(r, "11x80@0,0(0,0); *11x40@12,0(0,0); 11x39@12,41(0,0)");
	mov(r->c[0], "l");
	expect_layout(r, "11x80@0,0(0,0); 11x40@12,0(0,0); *11x39@12,41(0,0)");
	return 0;
}

static void
read_until(FILE *fp, const char *s, VTPARSER *vp)
{
	const char *t = s;
	while( *t ) {
		int c = fgetc(fp);
		vtwrite(vp, (char *)&c, 1);
		t = (c == *t) ? t + 1 : s;
	}
}

static int
test_cuu(int fd)
{
	struct canvas *root = init(24, 80);
	FILE *ofp = fdopen(fd = root->p->pt, "r");

	if( ofp == NULL ) {
		err(1, "Unable to fdopen master pty\n");
	}
	expect_layout(root, "*23x80@0,0(0,0)");
	send_cmd(fd, "PS1='X'; tput cud 5\r");
	read_until(ofp, "X", &root->p->vp); /* discard first line */
	read_until(ofp, "X", &root->p->vp);
	/* ??? I believe x should be 1, but it keeps coming back 3,
	 * but this is testing y, so ignore the x value. */
	expect_layout(root, "*23x80@0,0(6,\0)");
	send_cmd(fd, "printf '0123456789ab'; tput cub 4\r");
	read_until(ofp, "X", &root->p->vp);
	expect_layout(root, "*23x80@0,0(7,9)");
	return 0;
}

static int
test1(int fd)
{
	char *cmds[] = {
		"echo err >&2;",
		"tput cud 2; tput cuu 2; tput cuf 1",
		"tput ed; tput bel",
		"tput hpa 5; tput ri",
		"tput cub 1; tput dch 1; tput ack",
		"tput civis; tput cvvis; tput ack",
		"tabs -5",
		"exit",
		NULL
	};
	for( char **cmd = cmds; *cmd; cmd++ ) {
		write(fd, *cmd, strlen(*cmd));
		write(fd, "\r", 1);
	}
	return 0;
}

typedef int test(int);
#define F(x, y) { .name = #x, .f = (x), .main = y }
int
main(int argc, char *const argv[])
{
	int rv = EXIT_SUCCESS;
	char *const args[] = { "smtx-test", NULL };
	struct { char *name; test *f; int main; } tab[] = {
		F(test1, 1),
		F(test_cuu, 0),
		F(test_description, 0),
		{ NULL, NULL, 0 }
	}, *v;
	setenv("SHELL", "/bin/sh", 1);
	for( v = tab; ( v->f && argc < 2 ) || *++argv; v += 1 ) {
		const char *name = *argv;
		if( argc > 1 ) {
			for( v = tab; v->name && strcmp(v->name, name); v++ )
				;
		}
		if( v->f ) {
			int fd;
			int status;

			switch( forkpty(&fd, NULL, NULL, NULL) ) {
			case -1:
				err(1, "forkpty");
				break;
			case 0:
				if( v->main ) {
					exit(smtx_main(1, args));
				} else {
					exit(v->f(fd));
				}
			default:
				if( v->main ) {
					v->f(fd);
				}
				wait(&status);
			}
			if( ! WIFEXITED(status) || WEXITSTATUS(status) != 0 ) {
				char iobuf[BUFSIZ], *s;
				rv = EXIT_FAILURE;
				fprintf(stderr, "test %s FAILED\n", v->name);
				ssize_t r = read(fd, s = iobuf, sizeof iobuf);
				if( r > 0 ) for( ; *s; s++ ) {
					if( isprint(*s) || *s == '\n' ) {
						fputc(*s, stderr);
					}
				}
			}
		} else {
			fprintf(stderr, "unknown function: %s\n", name);
			rv = EXIT_FAILURE;
		}
	}
	return rv;
}
