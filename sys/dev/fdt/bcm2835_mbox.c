/*     $OpenBSD: bcm2835_mbox.c,v 1.5 2024/05/28 09:19:04 claudio Exp $ */

/*
 * Copyright (c) 2020 Tobias Heider <tobhe@openbsd.org>
 * Copyright (c) 2019 Neil Ashford <ashfordneil0@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*-
 * Copyright (c) 2012 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Nick Hudson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/mutex.h>

#include <machine/bus.h>
#include <machine/fdt.h>
#include <machine/intr.h>

#include <dev/ofw/fdt.h>
#include <dev/ofw/openfirm.h>

#include <dev/ic/bcm2835_mbox.h>
#include <dev/ic/bcm2835_vcprop.h>

#define DEVNAME(sc) ((sc)->sc_dev.dv_xname)

struct cfdriver bcmmbox_cd = { NULL, "bcmmbox", DV_DULL };

struct bcmmbox_softc {
	struct device sc_dev;
	bus_space_tag_t sc_iot;
	bus_space_handle_t sc_ioh;

	bus_dma_tag_t	sc_dmat;
	bus_dmamap_t	sc_dmamap;

	void *sc_ih;

	struct mutex sc_intr_lock;
	int sc_chan[BCMMBOX_NUM_CHANNELS];
	uint32_t sc_mbox[BCMMBOX_NUM_CHANNELS];
};

static struct bcmmbox_softc *bcmmbox_sc;

int bcmmbox_match(struct device *, void *, void *);
void bcmmbox_attach(struct device *, struct device *, void *);

const struct cfattach bcmmbox_ca = {
	sizeof(struct bcmmbox_softc),
	bcmmbox_match,
	bcmmbox_attach,
};

uint32_t bcmmbox_reg_read(struct bcmmbox_softc *, int);
void bcmmbox_reg_write(struct bcmmbox_softc *, int, uint32_t);
void bcmmbox_reg_flush(struct bcmmbox_softc *, int);
int bcmmbox_intr(void *);
int bcmmbox_intr_helper(struct bcmmbox_softc *, int);

int
bcmmbox_match(struct device *parent, void *match, void *aux)
{
	struct fdt_attach_args *faa = aux;

	return OF_is_compatible(faa->fa_node, "brcm,bcm2835-mbox");
}

void
bcmmbox_attach(struct device *parent, struct device *self, void *aux)
{
	struct bcmmbox_softc *sc = (struct bcmmbox_softc *)self;
	struct fdt_attach_args *faa = aux;
	bus_addr_t addr;
	bus_size_t size;

	if (bcmmbox_sc) {
		printf(": a similar device as already attached\n");
		return;
	}
	bcmmbox_sc = sc;

	mtx_init(&sc->sc_intr_lock, IPL_VM);

	if (faa->fa_nreg < 1) {
		printf(": no registers\n");
		return;
	}

	addr = faa->fa_reg[0].addr;
	size = faa->fa_reg[0].size;

	sc->sc_dmat = faa->fa_dmat;
	sc->sc_iot = faa->fa_iot;
	if (bus_space_map(sc->sc_iot, addr, size, 0, &sc->sc_ioh)) {
		printf(": can't map registers\n");
		return;
	}

	if (bus_dmamap_create(sc->sc_dmat, ~0UL, 1, ~0UL, 0,
	    BUS_DMA_NOWAIT|BUS_DMA_ALLOCNOW, &sc->sc_dmamap) != 0) {
		printf(": unable to create dma map\n");
		goto clean_bus_space_map;
	}

	sc->sc_ih = fdt_intr_establish(faa->fa_node, IPL_VM, bcmmbox_intr, sc,
	    DEVNAME(sc));
	if (sc->sc_ih == NULL) {
		printf(": failed to establish interrupt\n");
		goto clean_dmamap;
	}

	/* enable interrupt in hardware */
	bcmmbox_reg_write(sc, BCMMBOX_CFG, BCMMBOX_CFG_DATA_IRQ_EN);

	printf("\n");

	return;

 clean_dmamap:
	bus_dmamap_destroy(sc->sc_dmat, sc->sc_dmamap);

 clean_bus_space_map:
	bus_space_unmap(sc->sc_iot, sc->sc_ioh, size);
}

uint32_t
bcmmbox_reg_read(struct bcmmbox_softc *sc, int addr)
{
	return bus_space_read_4(sc->sc_iot, sc->sc_ioh, addr);
}

void
bcmmbox_reg_write(struct bcmmbox_softc *sc, int addr, uint32_t val)
{
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, addr, val);
}

void
bcmmbox_reg_flush(struct bcmmbox_softc *sc, int flags)
{
	bus_space_barrier(sc->sc_iot, sc->sc_ioh, 0, BCMMBOX_SIZE, flags);
}

int
bcmmbox_intr(void *cookie)
{
	struct bcmmbox_softc *sc = cookie;
	int ret;

	mtx_enter(&sc->sc_intr_lock);
	ret = bcmmbox_intr_helper(sc, 1);
	mtx_leave(&sc->sc_intr_lock);

	return ret;
}

int
bcmmbox_intr_helper(struct bcmmbox_softc *sc, int broadcast)
{
	uint32_t mbox, chan, data;
	int ret = 0;

	bcmmbox_reg_flush(sc, BUS_SPACE_BARRIER_READ);

	while (!ISSET(bcmmbox_reg_read(sc, BCMMBOX_STATUS), BCMMBOX_STATUS_EMPTY)) {
		mbox = bcmmbox_reg_read(sc, BCMMBOX0_READ);

		chan = mbox & BCMMBOX_CHANNEL_MASK;
		data = mbox & ~BCMMBOX_CHANNEL_MASK;
		ret = 1;

		if ((sc->sc_mbox[chan] & BCMMBOX_CHANNEL_MASK) != 0) {
			printf("%s: chan %d overflow\n", DEVNAME(sc), chan);
			continue;
		}

		sc->sc_mbox[chan] = data | BCMMBOX_CHANNEL_MASK;

		if (broadcast)
			wakeup(&sc->sc_chan[chan]);
	}

	return ret;
}

void
bcmmbox_read(uint8_t chan, uint32_t *data)
{
	struct bcmmbox_softc *sc = bcmmbox_sc;
	uint32_t mbox, rchan, rdata, status;

	KASSERT(sc != NULL);
	KASSERT(chan == (chan & BCMMBOX_CHANNEL_MASK));

	while (1) {
		bcmmbox_reg_flush(sc, BUS_SPACE_BARRIER_READ);
		status = bcmmbox_reg_read(sc, BCMMBOX0_STATUS);
		if (ISSET(status, BCMMBOX_STATUS_EMPTY))
			continue;

		mbox = bcmmbox_reg_read(sc, BCMMBOX0_READ);

		rchan = mbox & BCMMBOX_CHANNEL_MASK;
		rdata = mbox & ~BCMMBOX_CHANNEL_MASK;

		if (rchan == chan) {
			*data = rdata;
			return;
		}
	}
}

void
bcmmbox_write(uint8_t chan, uint32_t data)
{
	struct bcmmbox_softc *sc = bcmmbox_sc;
	uint32_t rdata;

	KASSERT(sc != NULL);
	KASSERT(chan == (chan & BCMMBOX_CHANNEL_MASK));
	KASSERT(data == (data & ~BCMMBOX_CHANNEL_MASK));

	while (1) {
		bcmmbox_reg_flush(sc, BUS_SPACE_BARRIER_READ);
		rdata = bcmmbox_reg_read(sc, BCMMBOX0_STATUS);
		if (!ISSET(rdata, BCMMBOX_STATUS_FULL))
			break;
	}

	bcmmbox_reg_write(sc, BCMMBOX1_WRITE, chan | data);
	bcmmbox_reg_flush(sc, BUS_SPACE_BARRIER_WRITE);
}

int
bcmmbox_post(uint8_t chan, void *buf, size_t len, uint32_t *res)
{
	struct bcmmbox_softc *sc = bcmmbox_sc;
	bus_dmamap_t map;
	int error;

	KASSERT(sc != NULL);
	if (sc == NULL)
		return (ENXIO);

	map = sc->sc_dmamap;

	error = bus_dmamap_load(sc->sc_dmat, map, buf, len, NULL,
	    BUS_DMA_NOWAIT | BUS_DMA_READ | BUS_DMA_WRITE);
	if (error != 0)
		return (error);

	bus_dmamap_sync(sc->sc_dmat, map, 0, len,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	bcmmbox_write(chan, map->dm_segs[0].ds_addr);
	bcmmbox_read(chan, res);

	bus_dmamap_sync(sc->sc_dmat, map, 0, len,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	bus_dmamap_unload(sc->sc_dmat, map);

	return (0);
}
