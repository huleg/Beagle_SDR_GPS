/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.
You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA  02110-1301, USA.
--------------------------------------------------------------------------------
*/

// Copyright (c) 2016 John Seamons, ZL/KF6VO

#include "types.h"
#include "config.h"
#include "kiwi.h"
#include "misc.h"
#include "timer.h"
#include "web.h"
#include "peri.h"
#include "spi.h"
#include "cfg.h"
#include "dx.h"
#include "coroutines.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>

// fixme: read from file, database etc.
// fixme: display depending on rx time-of-day

dxlist_t dx;

void dx_save_as_json()
{
	int i;
	cfg_t *cfg = &cfg_dx;
	dx_t *dxp;

	printf("saving as dx.json, %d entries\n", dx.len);

	cfg->json_size = dx.len * 256;		// FIXME XXX 256
	cfg->json = (char *) kiwi_malloc("dx json buf", cfg->json_size);
	char *cp = cfg->json;
	int n = sprintf(cp, "{\"dx\":["); cp += n;
	
	for (i=0, dxp = dx.list; i < dx.len; i++, dxp++) {
		n = sprintf(cp, "%s[%.2f", i? ",":"", dxp->freq); cp += n;
		n = sprintf(cp, ",\"%s\"", modu_s[dxp->flags & DX_MODE]); cp += n;
		char *ident = str_escape(dxp->ident);
		char *notes = str_escape(dxp->notes);
		n = sprintf(cp, ",\"%s\",\"%s\"", ident, notes); cp += n;
		kiwi_free("str_escape", ident);
		kiwi_free("str_escape", notes);
		assert(dxp->high_cut == 0);
		u4_t type = dxp->flags & DX_TYPE;
		if (type || dxp->offset) {
			const char *delim = ",{";
			if (type == WL) { n = sprintf(cp, "%s\"WL\":1", delim); cp += n; delim = ","; }
			if (type == SB) { n = sprintf(cp, "%s\"SB\":1", delim); cp += n; delim = ","; }
			if (type == DG) { n = sprintf(cp, "%s\"DG\":1", delim); cp += n; delim = ","; }
			if (type == NoN) { n = sprintf(cp, "%s\"NoN\":1", delim); cp += n; delim = ","; }
			if (type == XX) { n = sprintf(cp, "%s\"XX\":1", delim); cp += n; delim = ","; }
			if (dxp->offset) { n = sprintf(cp, "%s\"o\":%.0f", delim, dxp->offset); cp += n; delim = ","; }
			*cp++ = '}';
		}
		*cp++ = ']';
		*cp++ = '\n';
	}
	
	n = sprintf(cp, "]}"); cp += n;
	dxcfg_save_json(cfg->json);
}

static void switch_dx_list(dx_t *_dx_list, int _dx_list_len)
{
	qsort(_dx_list, _dx_list_len, sizeof(dx_t), qsort_floatcomp);
	
	// switch to new list
	dx_t *prev_dx_list = dx.list;
	int prev_dx_list_len = dx.len;
	dx.list = _dx_list;
	dx.len = _dx_list_len;
	dx.hidden_used = false;
	
	// release previous
	if (prev_dx_list) {
		int i;
		dx_t *dxp;
		for (i=0, dxp = prev_dx_list; i < prev_dx_list_len; i++, dxp++) {
			if (dxp->ident) free((void *) dxp->ident);
			if (dxp->notes) free((void *) dxp->notes);
		}
	}
	
	kiwi_free("dx_list", prev_dx_list);
}

static void dxcfg_mode(dx_t *dxp, const char *s)
{
	if (strcmp(s, "AM") == 0) dxp->flags = AM; else
	if (strcmp(s, "AMN") == 0) dxp->flags = AMN; else
	if (strcmp(s, "LSB") == 0) dxp->flags = LSB; else
	if (strcmp(s, "USB") == 0) dxp->flags = USB; else
	if (strcmp(s, "CW") == 0) dxp->flags = CW; else
	if (strcmp(s, "CWN") == 0) dxp->flags = CWN; else panic("dx config mode");
}

static void dxcfg_flag(dx_t *dxp, const char *flag)
{
	if (strcmp(flag, "WL") == 0) dxp->flags |= WL; else
	if (strcmp(flag, "SB") == 0) dxp->flags |= SB; else
	if (strcmp(flag, "DG") == 0) dxp->flags |= DG; else
	if (strcmp(flag, "NoN") == 0) dxp->flags |= NoN; else
	if (strcmp(flag, "XX") == 0) dxp->flags |= XX; else
	if (strcmp(flag, "PB") == 0) dxp->flags |= PB; else
	lprintf("%.2f \"%s\": unknown dx flag \"%s\"\n", dxp->freq, dxp->ident, flag);
}

static void dx_reload_json(cfg_t *cfg)
{
	const char *s;
	jsmntok_t *end_tok = &(cfg->tokens[cfg->ntok]);
	jsmntok_t *jt = cfg_lookup_json(cfg, "dx");
	assert(jt != NULL);
	assert(jt->type == JSMN_ARRAY);
	int _dx_list_len = jt->size;
	jt++;
	
	lprintf("%d dx entries\n", _dx_list_len);
	
	dx_t *_dx_list = (dx_t *) kiwi_malloc("dx_list", (_dx_list_len+1) * sizeof(dx_t));
	
	dx_t *dxp = _dx_list;
	int i = 0;

	for (; jt != end_tok; dxp++, i++) {
		assert(i < _dx_list_len);
		assert(jt->type == JSMN_ARRAY);
		jt++;
		
		double f;
		assert(cfg_float_json(cfg, jt, &f) == true);
		dxp->freq = f;
		jt++;
		
		const char *mode;
		assert(cfg_string_json(cfg, jt, &mode) == true);
		dxcfg_mode(dxp, mode);
		jt++;
		
		assert(cfg_string_json(cfg, jt, &s) == true);
		dxp->ident = s;
		kiwi_chrrep((char *) dxp->ident, '\'', '"');		// SECURITY: prevent Ajax reply escape
		jt++;
		
		assert(cfg_string_json(cfg, jt, &s) == true);
		dxp->notes = s;
		kiwi_chrrep((char *) dxp->notes, '\'', '"');		// SECURITY: prevent Ajax reply escape
		if (*dxp->notes == '\0')
			dxp->notes = NULL;		// because STREAM_DX leaves off argument to save space
		jt++;
		
		//printf("dx.json %d %.2f %s \"%s\" \"%s\"\n", i, dxp->freq, mode, dxp->ident, dxp->notes);

		if (jt->type == JSMN_OBJECT) {
			jt++;
			while (jt->type != JSMN_ARRAY) {
				assert(jt->size == 1);
				const char *id;
				assert(cfg_string_json(cfg, jt, &id) == true);
				jt++;
				int num;
				assert(cfg_int_json(cfg, jt, &num) == true);
				if (strcmp(id, "o") == 0) {
					dxp->offset = num;
					//printf("dx.json %d %s %d\n", i, id, num);
				} else {
					if (num) {
						dxcfg_flag(dxp, id);
						//printf("dx.json %d %s\n", i, id);
					}
				}
				jt++;
			}
		}
	}

	switch_dx_list(_dx_list, _dx_list_len);
}

static void dx_reload_cfg()
{
	int i, j;
	const char *s;

	config_setting_t *dxs = dxcfg_lookup("dx", CFG_REQUIRED);
	assert(config_setting_type(dxs) == CONFIG_TYPE_LIST);
	
	const config_setting_t *dxe;
	for (i=0; (dxe = config_setting_get_elem(dxs, i)) != NULL; i++) {
		assert(config_setting_type(dxe) == CONFIG_TYPE_GROUP);
	}
	int _dx_list_len = i-1;
	lprintf("%d dx entries\n", _dx_list_len);
	
	dx_t *_dx_list = (dx_t *) kiwi_malloc("dx_list", (_dx_list_len+1) * sizeof(dx_t));
	
	float f = 0;
	
	dx_t *dxp;
	for (i=0, dxp = _dx_list; i < _dx_list_len; i++, dxp++) {
		dxe = config_setting_get_elem(dxs, i);
		
		config_setting_t *e;
		assert((e = config_setting_get_member(dxe, "e")) != NULL);
		assert(config_setting_type(e) == CONFIG_TYPE_LIST);
		
		assert((dxp->freq = (float) config_setting_get_float_elem(e, 0)) != 0);
		if (dxp->freq < f)
			lprintf(">>>> DX: entry with freq %.2f < current freq %.2f\n", dxp->freq, f);
		else
			f = dxp->freq;

		assert((s = config_setting_get_string_elem(e, 1)) != NULL);
		dxcfg_mode(dxp, s);
		
		assert((s = config_setting_get_string_elem(e, 2)) != NULL);
		dxp->ident = strdup(s);
		kiwi_chrrep((char *) dxp->ident, '\'', '"');		// SECURITY: prevent Ajax reply escape
		
		if ((s = config_setting_get_string_elem(e, 3)) == NULL) {
			dxp->notes = NULL;
		} else {
			dxp->notes = strdup(s);
			kiwi_chrrep((char *) dxp->notes, '\'', '"');		// SECURITY: prevent Ajax reply escape
		}

		config_setting_t *flags;
		const char *flag;
		if ((flags = config_setting_get_member(dxe, "f")) != NULL) {
			if (config_setting_type(flags) == CONFIG_TYPE_ARRAY) {
				for (j=0; j < config_setting_length(flags); j++) {
					assert((flag = config_setting_get_string_elem(flags, j)) != NULL);
					dxcfg_flag(dxp, flag);
				}
			} else {
				assert((flag = config_setting_get_string(flags)) != NULL);
				dxcfg_flag(dxp, flag);
			}
		}

		config_setting_t *offset;
		if ((offset = config_setting_get_member(dxe, "o")) != NULL) {
			if (config_setting_type(offset) == CONFIG_TYPE_ARRAY) {
				assert((dxp->low_cut = (float) config_setting_get_int_elem(offset, 0)) != 0);
				assert((dxp->high_cut = (float) config_setting_get_int_elem(offset, 1)) != 0);
			} else {
				assert((dxp->offset = (float) config_setting_get_int(offset)) != 0);
			}
		}

//printf("dxe %d f %.2f notes-%c off %.0f,%.0f\n", i, dxp->freq, dxp->notes? 'Y':'N', dxp->offset, dxp->high_cut);
	}
	
	switch_dx_list(_dx_list, _dx_list_len);

	// convert to json	
	dx_save_as_json();
}

// reload requested, at startup or when file edited by hand
void dx_reload()
{
	cfg_t *cfg = &cfg_dx;
	
	dxcfg_init();
		
	if (cfg->use_json) {
		//dxcfg_walk(NULL, cfg_print_tok);
		dx_reload_json(cfg);
	} else {
		dx_reload_cfg();
	}
}
