/*****************************************************************************
 * RRDtool 1.4.3  Copyright by Tobi Oetiker, 1997-2010
 *****************************************************************************
 * rrd_tool.c  Startup wrapper
 *****************************************************************************/

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <fcntl.h>

#include "rrd_tool.h"
#include "rrd_xport.h"
#include "rrd_i18n.h"

#include <locale.h>


#define TRUE		1
#define FALSE		0
#define MAX_LENGTH	10000


static char *fgetslong(
	char **aLinePtr,
	FILE * stream)
{
	char	 *linebuf;
	size_t	  bufsize = MAX_LENGTH;
	int		  eolpos = 0;

	if (feof(stream))
		return *aLinePtr = 0;
	if (!(linebuf = (char *) malloc(bufsize))) {
		perror("fgetslong: malloc");
		exit(1);
	}
	linebuf[0] = '\0';
	while (fgets(linebuf + eolpos, MAX_LENGTH, stream)) {
		eolpos += strlen(linebuf + eolpos);
		if (linebuf[eolpos - 1] == '\n')
			return *aLinePtr = linebuf;
		bufsize += MAX_LENGTH;
		if (!(linebuf = (char *) realloc(linebuf, bufsize))) {
			free(linebuf);
			perror("fgetslong: realloc");
			exit(1);
		}
	}
	
	if (linebuf[0]){
		return	*aLinePtr = linebuf;
	}
	free(linebuf);
	return *aLinePtr = 0;
}


/* HandleInputLine is NOT thread safe - due to readdir issues,
   resolving them portably is not really simple. */
static int HandleInputLine(
	int argc,
	char **argv,
	FILE * out)
{
#if defined(HAVE_OPENDIR) && defined (HAVE_READDIR)
	DIR		 *curdir;	/* to read current dir with ls */
	struct dirent *dent;
#endif

	if (argc < 3
		|| strcmp("help", argv[1]) == 0
		|| strcmp("--help", argv[1]) == 0
		|| strcmp("-help", argv[1]) == 0
		|| strcmp("-?", argv[1]) == 0 || strcmp("-h", argv[1]) == 0) {
		return 0;
	}

	if (strcmp("create", argv[1]) == 0)
		rrd_create(argc - 1, &argv[1]);
	else if (strcmp("dump", argv[1]) == 0)
		rrd_dump(argc - 1, &argv[1]);
	else if (strcmp("info", argv[1]) == 0 || strcmp("updatev", argv[1]) == 0) {
		rrd_info_t *data;

		if (strcmp("info", argv[1]) == 0)

			data = rrd_info(argc - 1, &argv[1]);
		else
			data = rrd_update_v(argc - 1, &argv[1]);
		rrd_info_print(data);
		rrd_info_free(data);
	}

	else if (strcmp("--version", argv[1]) == 0 ||
			 strcmp("version", argv[1]) == 0 ||
			 strcmp("v", argv[1]) == 0 ||
			 strcmp("-v", argv[1]) == 0 || strcmp("-version", argv[1]) == 0)
		printf("RRDtool " PACKAGE_VERSION
			   "  Copyright by Tobi Oetiker, 1997-2008 (%f)\n",
			   rrd_version());
	else if (strcmp("restore", argv[1]) == 0)
		rrd_restore(argc - 1, &argv[1]);
	else if (strcmp("resize", argv[1]) == 0)
		rrd_resize(argc - 1, &argv[1]);
	else if (strcmp("last", argv[1]) == 0)
		printf("%ld\n", rrd_last(argc - 1, &argv[1]));
	else if (strcmp("lastupdate", argv[1]) == 0) {
		rrd_lastupdate(argc - 1, &argv[1]);
	} else if (strcmp("first", argv[1]) == 0)
		printf("%ld\n", rrd_first(argc - 1, &argv[1]));
	else if (strcmp("update", argv[1]) == 0)
		rrd_update(argc - 1, &argv[1]);
	else if (strcmp("fetch", argv[1]) == 0) {
		time_t	  start, end, ti;
		unsigned long step, ds_cnt, i, ii;
		rrd_value_t *data, *datai;
		char	**ds_namv;

		if (rrd_fetch
			(argc - 1, &argv[1], &start, &end, &step, &ds_cnt, &ds_namv,
			 &data) == 0) {
			datai = data;
			printf("		   ");
			for (i = 0; i < ds_cnt; i++)
				printf("%20s", ds_namv[i]);
			printf("\n\n");
			for (ti = start + step; ti <= end; ti += step) {
				printf("%10lu:", ti);
				for (ii = 0; ii < ds_cnt; ii++)
					printf(" %0.10e", *(datai++));
				printf("\n");
			}
			for (i = 0; i < ds_cnt; i++)
				free(ds_namv[i]);
			free(ds_namv);
			free(data);
		}
	} else if (strcmp("xport", argv[1]) == 0) {
#ifdef HAVE_RRD_GRAPH
	  time_t	start, end;
	  unsigned long step, col_cnt;
	  rrd_value_t *data;
	  char	  **legend_v;
	  rrd_xport
	(argc - 1, &argv[1], NULL, &start, &end, &step, &col_cnt,
	 &legend_v, &data);
#else
		rrd_set_error("the instance of rrdtool has been compiled without graphics");
#endif
	} else if (strcmp("graph", argv[1]) == 0) {
#ifdef HAVE_RRD_GRAPH
		char	**calcpr;

#ifdef notused /*XXX*/
		const char *imgfile = argv[2];	/* rrd_graph changes argv pointer */
#endif
		int		  xsize, ysize;
		double	  ymin, ymax;
		int		  i;
		int		  tostdout = (strcmp(argv[2], "-") == 0);
		int		  imginfo = 0;

		for (i = 2; i < argc; i++) {
			if (strcmp(argv[i], "--imginfo") == 0
				|| strcmp(argv[i], "-f") == 0) {
				imginfo = 1;
				break;
			}
		}
		if (rrd_graph
			(argc - 1, &argv[1], &calcpr, &xsize, &ysize, NULL, &ymin,
			 &ymax) == 0) {
			if (!tostdout && !imginfo)
				printf("%dx%d\n", xsize, ysize);
			if (calcpr) {
				for (i = 0; calcpr[i]; i++) {
					if (!tostdout)
						printf("%s\n", calcpr[i]);
					free(calcpr[i]);
				}
				free(calcpr);
			}
		}

#else
	   rrd_set_error("the instance of rrdtool has been compiled without graphics");
#endif
	} else if (strcmp("graphv", argv[1]) == 0) {
#ifdef HAVE_RRD_GRAPH
		rrd_info_t *grinfo = NULL;	/* 1 to distinguish it from the NULL that rrd_graph sends in */

		grinfo = rrd_graph_v(argc - 1, &argv[1]);
		if (grinfo) {
			rrd_info_print(grinfo);
			rrd_info_free(grinfo);
		}
#else
	   rrd_set_error("the instance of rrdtool has been compiled without graphics");
#endif
	} else if (strcmp("tune", argv[1]) == 0)
		rrd_tune(argc - 1, &argv[1]);
	else if (strcmp("flushcached", argv[1]) == 0)
		rrd_flushcached(argc - 1, &argv[1]);
	else {
		rrd_set_error("unknown function '%s'", argv[1]);
	}
	if (rrd_test_error()) {
		fprintf(out, "ERROR: %s\n", rrd_get_error());
		rrd_clear_error();
		return 1;
	}
	return (0);
}

static int CountArgs(
	char *aLine)
{
	int		  i = 0;
	int		  aCount = 0;
	int		  inarg = 0;

	while (aLine[i] == ' ')
		i++;
	while (aLine[i] != 0) {
		if ((aLine[i] == ' ') && inarg) {
			inarg = 0;
		}
		if ((aLine[i] != ' ') && !inarg) {
			inarg = 1;
			aCount++;
		}
		i++;
	}
	return aCount;
}

/*
 * CreateArgs - take a string (aLine) and tokenize
 */
static int CreateArgs(
	char *pName,
	char *aLine,
	char **argv)
{
	char	 *getP, *putP;
	char	**pargv = argv;
	char	  Quote = 0;
	int		  inArg = 0;
	int		  len;
	int		  argc = 1;

	len = strlen(aLine);
	/* remove trailing space and newlines */
	while (len && aLine[len] <= ' ') {
		aLine[len] = 0;
		len--;
	}
	/* sikp leading blanks */
	while (*aLine && *aLine <= ' ')
		aLine++;

	pargv[0] = pName;
	argc = 1;
	getP = aLine;
	putP = aLine;
	while (*getP) {
		switch (*getP) {
		case ' ':
			if (Quote) {
				*(putP++) = *getP;
			} else if (inArg) {
				*(putP++) = 0;
				inArg = 0;
			}
			break;
		case '"':
		case '\'':
			if (Quote != 0) {
				if (Quote == *getP)
					Quote = 0;
				else {
					*(putP++) = *getP;
				}
			} else {
				if (!inArg) {
					pargv[argc++] = putP;
					inArg = 1;
				}
				Quote = *getP;
			}
			break;
		default:
			if (!inArg) {
				pargv[argc++] = putP;
				inArg = 1;
			}
			*(putP++) = *getP;
			break;
		}
		getP++;
	}

	*putP = '\0';
	int i=0;
	while (pargv[i]) {
		printf("Arg:%d = %s\n",i,pargv[i]);
		i++;
	}

	if (Quote)
		return -1;
	else
		return argc;
}


int rrd_call(
	char *aLine
	)
{
	int myargc;
	char *tmpLine;
	char **myargv;

	if ((myargc = CountArgs(aLine)) == 0) {
		printf("RRD_CALL ERROR: not enough arguments\n");
		return (1);
	}
//	printf ("CountArgs vratil %d\n",myargc);

	if ((tmpLine = (char *) malloc((strlen(aLine) + 1) *
								   sizeof(char *))) == NULL) {
		perror("malloc");
		return (1);
	}
	if ((myargv = (char **) malloc((myargc + 1) *
								   sizeof(char *))) == NULL) {
		perror("malloc");
		return (1);
	}

	memcpy(tmpLine, aLine, strlen(aLine) - 1);
	tmpLine[strlen(aLine)] = '\0';

	if ((myargc = CreateArgs("./rrdtool", tmpLine, myargv)) > 0) {
		int result = HandleInputLine(myargc, myargv, stderr);
		free(myargv);
		return (result);
	} else {
		return (-1);
	}
}



