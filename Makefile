PROG= v4lliveview
SRCS= src/v4lliveview.c
MAN=

.if defined(BACKGROUND)
BACKGROUND_PORTS?= 4
CFLAGS+= -DV4LLIVEVIEW_BACKGROUND -DV4LLIVEVIEW_PORTS=${BACKGROUND_PORTS}
.endif

CFLAGS+= -I/usr/local/include -std=c11 -Wall -Wextra -Wpedantic -Werror \
    -Wno-c11-extensions -O2
LDFLAGS+= -L/usr/local/lib
LDADD+= -lcuse -lX11 -lXext -lpthread -ljpeg

.include <bsd.prog.mk>
