CC?= cc
CFLAGS?= -O2
CPPFLAGS+= -I/usr/local/include
CFLAGS+= -std=c11 -Wall -Wextra -Wpedantic -Werror -Wno-c11-extensions
LDFLAGS+= -L/usr/local/lib
BACKGROUND_PORTS?= 4

.PHONY: all clean
.if defined(BACKGROUND)
all: v4lliveview-background
.else
all: v4lliveview v4lliveview-background
.endif

# Separate direct builds prevent stale objects when switching compile modes.
v4lliveview: src/v4lliveview.c Makefile
	${CC} ${CPPFLAGS} ${CFLAGS} ${LDFLAGS} -o ${.TARGET} src/v4lliveview.c -lcuse -lX11 -lXext -ljpeg -lpthread

v4lliveview-background: src/v4lliveview.c Makefile
	${CC} ${CPPFLAGS} ${CFLAGS} -DV4LLIVEVIEW_BACKGROUND -DV4LLIVEVIEW_PORTS=${BACKGROUND_PORTS} ${LDFLAGS} -o ${.TARGET} src/v4lliveview.c -lcuse -lpthread

v4lliveview-safety: src/v4lliveview.c Makefile
	${CC} ${CPPFLAGS} ${CFLAGS} -DV4LLIVEVIEW_BACKGROUND -DV4LLIVEVIEW_SAFETY -DV4LLIVEVIEW_PORTS=2 ${LDFLAGS} -o ${.TARGET} src/v4lliveview.c -lcuse -lpthread

clean:
	rm -f v4lliveview v4lliveview-background v4lliveview-safety
