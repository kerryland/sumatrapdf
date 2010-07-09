/*
 * Benchmarking of loading and drawing pdf pages.
 */

#ifdef WIN32
#include <windows.h>
#endif

#include "fitz.h"
#include "mupdf.h"

#define logbench(...) \
	printf(__VA_ARGS__); \
	fflush(stdout)

int pagetobench = -1;
int loadonly = 0;
pdf_xref *xref = nil;
int pagecount = 0;
fz_glyphcache *drawcache = nil;
pdf_page *drawpage = nil;

/* milli-second timer */
#ifdef _WIN32
typedef struct mstimer {
    LARGE_INTEGER   start;
    LARGE_INTEGER   end;
} mstimer;

void timerstart(mstimer *timer)
{
    assert(timer);
    if (!timer)
        return;
    QueryPerformanceCounter(&timer->start);
}
void timerstop(mstimer *timer)
{
    assert(timer);
    if (!timer)
        return;
    QueryPerformanceCounter(&timer->end);
}

double timeinms(mstimer *timer)
{
    LARGE_INTEGER   freq;
    double          time_in_secs;
    QueryPerformanceFrequency(&freq);
    time_in_secs = (double)(timer->end.QuadPart-timer->start.QuadPart)/(double)freq.QuadPart;
    return time_in_secs * 1000.0;
}
#else
#include <sys/time.h>
typedef struct mstimer {
    struct timeval    start;
    struct timeval    end;
} mstimer;

void timerstart(mstimer *timer)
{
    assert(timer);
    if (!timer)
        return;
    gettimeofday(&timer->start, NULL);
}

void timerstop(mstimer *timer)
{
    assert(timer);
    if (!timer)
        return;
    gettimeofday(&timer->end, NULL);
}

double timeinms(mstimer *timer)
{
    double timeInMs;
    time_t seconds;
    int    usecs;

    assert(timer);
    if (!timer)
        return 0.0;
    seconds = timer->end.tv_sec - timer->start.tv_sec;
    usecs = timer->end.tv_usec - timer->start.tv_usec;
    if (usecs < 0) {
        --seconds;
        usecs += 1000000;
    }
    timeInMs = (double)seconds*(double)1000.0 + (double)usecs/(double)1000.0;
    return timeInMs;
}
#endif

void closexref(void)
{
	if (!xref)
		return;

	if (xref->store)
	{
		pdf_freestore(xref->store);
		xref->store = nil;
	}
	pdf_closexref(xref);
	xref = nil;
}

fz_error openxref(char *filename, char *password)
{
	fz_stream *file = fz_openfile(open(filename, O_BINARY | O_RDONLY, 0666));
	xref = pdf_openxref(file);
	fz_dropstream(file);
	if (!xref)
	{
		return fz_throw("pdf_openxref() failed");
	}

	if (pdf_needspassword(xref))
	{
		int okay = pdf_authenticatepassword(xref, password);
		if (!okay)
		{
			logbench("Warning: pdf_setpassword() failed, incorrect password\n");
			return fz_throw("invalid password");
		}
	}

	pagecount = pdf_getpagecount(xref);

	return fz_okay;
}

fz_error benchloadpage(int pagenum)
{
	fz_error error;
	fz_obj *pageobj;
	mstimer timer;
	double timems;

	timerstart(&timer);
	pageobj = pdf_getpageobject(xref, pagenum);

	drawpage = nil;
	error = pdf_loadpage(&drawpage, xref, pageobj);
	timerstop(&timer);
	if (error)
	{
		logbench("Error: failed to load page %d\n", pagenum);
		return error;
	}
	timems = timeinms(&timer);
	logbench("pageload   %3d: %.2f ms\n", pagenum, timems);
	return fz_okay;
}

fz_error benchrenderpage(int pagenum)
{
	fz_error error;
	fz_matrix ctm;
	fz_bbox bbox;
	fz_pixmap *pix;
	int w, h;
	mstimer timer;
	double timems;
	fz_device *dev;

	timerstart(&timer);
	ctm = fz_identity;
	ctm = fz_concat(ctm, fz_translate(0, -drawpage->mediabox.y1));

	bbox = fz_roundrect(fz_transformrect(ctm, drawpage->mediabox));
	w = bbox.x1 - bbox.x0;
	h = bbox.y1 - bbox.y0;

	pix = fz_newpixmapwithrect(pdf_devicergb, bbox);
	fz_clearpixmap(pix, 0xFF);
	dev = fz_newdrawdevice(drawcache, pix);
	error = pdf_runcontentstream(dev, ctm, xref, drawpage->resources, drawpage->contents);
	fz_freedevice(dev);
	if (error)
	{
		logbench("Error: pdf_runcontentstream() failed\n");
		goto Exit;
	}

	timerstop(&timer);
	timems = timeinms(&timer);

	logbench("pagerender %3d: %.2f ms\n", pagenum, timems);
Exit:
	fz_droppixmap(pix);
	return fz_okay;
}

void freepage(void)
{
	pdf_freepage(drawpage);
	drawpage = nil;
}

void benchfile(char *pdffilename)
{
	fz_error error;
	mstimer timer;
	double timems;
	int pages;
	int curpage;

	drawcache = fz_newglyphcache();
	if (!drawcache)
	{
		logbench("Error: fz_newglyphcache() failed\n");
		goto Exit;
	}

	logbench("Starting: %s\n", pdffilename);
	timerstart(&timer);
	error = openxref(pdffilename, "");
	timerstop(&timer);
	if (error)
		goto Exit;
	timems = timeinms(&timer);
	logbench("load: %.2f ms\n", timems);

	pages = pagecount;
	logbench("page count: %d\n", pages);

	if (loadonly)
		goto Exit;
	for (curpage = 1; curpage <= pages; curpage++) {
		if ((-1 != pagetobench) && (pagetobench != curpage))
			continue;
		error = benchloadpage(curpage);
		if (!error)
			benchrenderpage(curpage);
		if (drawpage)
			freepage();
	}

Exit:
	logbench("Finished: %s\n", pdffilename);
	if (drawcache)
		fz_freeglyphcache(drawcache);
	closexref();
}

void usage(void)
{
	fprintf(stderr, "usage: pdfbench [-loadonly] [-page N] <pdffile>\n");
	exit(1);
}

int isarg(char *arg, char *name)
{
	if ('-' != *arg++)
		return 0;
	/* be liberal, allow '-' and '--' as arg prefix */
	if ('-'== *arg) ++arg;
	return (0 == strcmp(arg, name));
}

void parsecmdargs(int argc, char **argv)
{
	int i;
	char *arg;

	if (argc < 2)
		usage();

	for (i=1; i<argc; i++) {
		arg = argv[i];	
		if (isarg(arg, "loadonly")) {
			loadonly = 1;
		}
		if (isarg(arg, "page")) {
			++i;
			if (i == argc)
				usage();
			pagetobench = atoi(argv[i]);
		}
	}
}

int main(int argc, char **argv)
{
	parsecmdargs(argc, argv);
	/* for simplicity assume the file to parse is always the last */
	benchfile(argv[argc-1]);
	return 0;
}
