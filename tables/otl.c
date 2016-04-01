#include "otl.h"

table_otl *caryll_new_otl() {
	table_otl *table;
	NEW(table);
	table->languageCount = 0;
	table->languages = NULL;
	table->featureCount = 0;
	table->features = NULL;
	table->lookupCount = 0;
	table->lookups = NULL;
	return table;
}
void caryll_delete_coverage(otl_coverage *coverage) {
	if (coverage && coverage->glyphs) free(coverage->glyphs);
	if (coverage) free(coverage);
}

void caryll_delete_otl(table_otl *table) {
	if (!table) return;
	if (table->languages) {
		for (uint16_t j = 0; j < table->languageCount; j++) {
			if (table->languages[j].name) sdsfree(table->languages[j].name);
			if (table->languages[j].features) free(table->languages[j].features);
		}
		free(table->languages);
	}
	if (table->features) {
		for (uint16_t j = 0; j < table->featureCount; j++) {
			if (table->features[j].name) sdsfree(table->features[j].name);
			if (table->features[j].lookups) free(table->features[j].lookups);
		}
		free(table->features);
	}
	if (table->lookups) {
		for (uint16_t j = 0; j < table->lookupCount; j++) {
			switch (table->lookups[j]->type) {
				case otl_type_gsub_single:
					caryll_delete_gsub_single(table->lookups[j]);
				default:
					break;
			}
		}
		free(table->lookups);
	}
	free(table);
}

#define checkLength(offset)                                                                                            \
	if (tableLength < offset) { goto FAIL; }

void parseLanguage(font_file_pointer data, uint32_t tableLength, uint32_t base, otl_language_system *lang,
                   uint16_t featureCount, otl_feature *features) {
	checkLength(base + 6);
	uint16_t rid = caryll_blt16u(data + base + 2);
	if (rid < featureCount) {
		lang->requiredFeature = &(features[rid]);
	} else {
		lang->requiredFeature = NULL;
	}
	lang->featureCount = caryll_blt16u(data + base + 4);
	checkLength(base + 6 + lang->featureCount * 2);

	NEW_N(lang->features, lang->featureCount);
	for (uint16_t j = 0; j < lang->featureCount; j++) {
		uint16_t featureIndex = caryll_blt16u(data + base + 6 + 2 * j);
		if (featureIndex < featureCount) {
			lang->features[j] = &(features[featureIndex]);
		} else {
			lang->features[j] = NULL;
		}
	}
	return;
FAIL:
	if (lang->features) free(lang->features);
	lang->featureCount = 0;
	lang->requiredFeature = NULL;
	return;
}

table_otl *caryll_read_otl_common(font_file_pointer data, uint32_t tableLength, otl_lookup_type lookup_type_base) {
	table_otl *table = caryll_new_otl();
	if (!table) goto FAIL;
	checkLength(10);
	uint32_t scriptListOffset = caryll_blt16u(data + 4);
	checkLength(scriptListOffset + 2);
	uint32_t featureListOffset = caryll_blt16u(data + 6);
	checkLength(featureListOffset + 2);
	uint32_t lookupListOffset = caryll_blt16u(data + 8);
	checkLength(lookupListOffset + 2);

	// parse lookup list
	{
		uint16_t lookupCount = caryll_blt16u(data + lookupListOffset);
		checkLength(lookupListOffset + 2 + lookupCount * 2);
		otl_lookup **lookups;
		NEW_N(lookups, lookupCount);
		for (uint16_t j = 0; j < lookupCount; j++) {
			NEW(lookups[j]);
			lookups[j]->name = NULL;
			lookups[j]->_offset = lookupListOffset + caryll_blt16u(data + lookupListOffset + 2 + 2 * j);
			checkLength(lookups[j]->_offset + 6);
			lookups[j]->type = caryll_blt16u(data + lookups[j]->_offset) + lookup_type_base;
		}
		table->lookupCount = lookupCount;
		table->lookups = lookups;
	}

	// parse feature list
	{
		uint16_t featureCount = caryll_blt16u(data + featureListOffset);
		checkLength(featureListOffset + 2 + featureCount * 6);
		otl_feature *features;
		NEW_N(features, featureCount);
		for (uint16_t j = 0; j < featureCount; j++) {
			uint32_t tag = caryll_blt32u(data + featureListOffset + 2 + j * 6);
			features[j].name = sdscatprintf(sdsempty(), "%c%c%c%c_%d", (tag >> 24) & 0xFF, (tag >> 16) & 0xFF,
			                                (tag >> 8) & 0xff, tag & 0xff, j);
			uint32_t featureOffset = featureListOffset + caryll_blt16u(data + featureListOffset + 2 + j * 6 + 4);

			checkLength(featureOffset + 4);
			uint16_t lookupCount = caryll_blt16u(data + featureOffset + 2);
			checkLength(featureOffset + 4 + lookupCount * 2);
			features[j].lookupCount = lookupCount;
			NEW_N(features[j].lookups, lookupCount);
			for (uint16_t k = 0; k < lookupCount; k++) {
				uint16_t lookupid = caryll_blt16u(data + featureOffset + 4 + k * 2);
				if (lookupid < table->lookupCount) {
					features[j].lookups[k] = table->lookups[lookupid];
					if (!features[j].lookups[k]->name) {
						features[j].lookups[k]->name =
						    sdscatprintf(sdsempty(), "lookup_%c%c%c%c_%d", (tag >> 24) & 0xFF, (tag >> 16) & 0xFF,
						                 (tag >> 8) & 0xff, tag & 0xff, k);
					}
				}
			}
		}
		table->featureCount = featureCount;
		table->features = features;
	}

	// parse script list
	{
		uint16_t scriptCount = caryll_blt16u(data + scriptListOffset);
		checkLength(scriptListOffset + 2 + 6 * scriptCount);

		uint32_t nLanguageCombinations = 0;
		for (uint16_t j = 0; j < scriptCount; j++) {
			uint32_t scriptOffset = scriptListOffset + caryll_blt16u(data + scriptListOffset + 2 + 6 * j + 4);
			checkLength(scriptOffset + 4);

			uint16_t defaultLangSystem = caryll_blt16u(data + scriptOffset);
			nLanguageCombinations += (defaultLangSystem ? 1 : 0) + caryll_blt16u(data + scriptOffset + 2);
		}

		table->languageCount = nLanguageCombinations;

		otl_language_system *languages;
		NEW_N(languages, nLanguageCombinations);

		uint16_t currentLang = 0;
		for (uint16_t j = 0; j < scriptCount; j++) {
			uint32_t tag = caryll_blt32u(data + scriptListOffset + 2 + 6 * j);
			uint32_t scriptOffset = scriptListOffset + caryll_blt16u(data + scriptListOffset + 2 + 6 * j + 4);
			uint16_t defaultLangSystem = caryll_blt16u(data + scriptOffset);
			if (defaultLangSystem) {
				languages[currentLang].name = sdscatprintf(sdsempty(), "%c%c%c%c_DFLT", (tag >> 24) & 0xFF,
				                                           (tag >> 16) & 0xFF, (tag >> 8) & 0xff, tag & 0xff);
				parseLanguage(data, tableLength, scriptOffset + defaultLangSystem, &(languages[currentLang]),
				              table->featureCount, table->features);
				currentLang += 1;
			}
			uint16_t langSysCount = caryll_blt16u(data + scriptOffset + 2);
			for (uint16_t k = 0; k < langSysCount; k++) {
				uint32_t langTag = caryll_blt32u(data + scriptOffset + 4 + 6 * k);
				uint16_t langSys = caryll_blt16u(data + scriptOffset + 4 + 6 * k + 4);
				languages[currentLang].name = sdscatprintf(
				    sdsempty(), "%c%c%c%c_%c%c%c%c", (tag >> 24) & 0xFF, (tag >> 16) & 0xFF, (tag >> 8) & 0xff,
				    tag & 0xff, (langTag >> 24) & 0xFF, (langTag >> 16) & 0xFF, (langTag >> 8) & 0xff, langTag & 0xff);
				parseLanguage(data, tableLength, scriptOffset + langSys, &(languages[currentLang]), table->featureCount,
				              table->features);
				currentLang += 1;
			}
		}

		table->languages = languages;
	}
	// name all lookups
	for (uint16_t j = 0; j < table->lookupCount; j++) {
		if (!table->lookups[j]->name)
			table->lookups[j]->name = sdscatprintf(sdsempty(), "lookup_%02x_%d", table->lookups[j]->type, j);
	}
	return table;
FAIL:
	if (table) caryll_delete_otl(table);
	return NULL;
}

typedef struct {
	int gid;
	int covIndex;
	UT_hash_handle hh;
} coverage_entry;

int by_covIndex(coverage_entry *a, coverage_entry *b) {
	return a->covIndex - b->covIndex;
}

otl_coverage *caryll_read_coverage(font_file_pointer data, uint32_t tableLength, uint32_t offset) {
	otl_coverage *coverage;
	NEW(coverage);
	coverage->numGlyphs = 0;
	coverage->glyphs = NULL;
	if (tableLength < offset + 4) return coverage;
	uint16_t format = caryll_blt16u(data + offset);
	switch (format) {
		case 1: {
			uint16_t glyphCount = caryll_blt16u(data + offset + 2);
			if (tableLength < offset + 4 + glyphCount * 2) return coverage;
			coverage_entry *hash = NULL;
			for (uint16_t j = 0; j < glyphCount; j++) {
				coverage_entry *item = NULL;
				int gid = caryll_blt16u(data + offset + 4 + j * 2);
				HASH_FIND_INT(hash, &gid, item);
				if (!item) {
					NEW(item);
					item->gid = gid;
					item->covIndex = j;
					HASH_ADD_INT(hash, gid, item);
				}
			}
			HASH_SORT(hash, by_covIndex);
			coverage->numGlyphs = HASH_COUNT(hash);
			NEW_N(coverage->glyphs, coverage->numGlyphs);
			{
				uint16_t j = 0;
				coverage_entry *e, *tmp;
				HASH_ITER(hh, hash, e, tmp) {
					coverage->glyphs[j].gid = e->gid;
					coverage->glyphs[j].name = NULL;
					HASH_DEL(hash, e);
					free(e);
					j++;
				}
			}
			break;
		}
		case 2: {
			uint16_t rangeCount = caryll_blt16u(data + offset + 2);
			if (tableLength < offset + 4 + rangeCount * 6) return coverage;
			coverage_entry *hash = NULL;
			for (uint16_t j = 0; j < rangeCount; j++) {
				uint16_t start = caryll_blt16u(data + offset + 4 + 6 * j);
				uint16_t end = caryll_blt16u(data + offset + 4 + 6 * j + 2);
				uint16_t startCoverageIndex = caryll_blt16u(data + offset + 4 + 6 * j + 4);
				for (int k = start; k <= end; k++) {
					coverage_entry *item = NULL;
					HASH_FIND_INT(hash, &k, item);
					if (!item) {
						NEW(item);
						item->gid = k;
						item->covIndex = startCoverageIndex + k;
						HASH_ADD_INT(hash, gid, item);
					}
				}
			}
			HASH_SORT(hash, by_covIndex);
			coverage->numGlyphs = HASH_COUNT(hash);
			NEW_N(coverage->glyphs, coverage->numGlyphs);
			{
				uint16_t j = 0;
				coverage_entry *e, *tmp;
				HASH_ITER(hh, hash, e, tmp) {
					coverage->glyphs[j].gid = e->gid;
					coverage->glyphs[j].name = NULL;
					HASH_DEL(hash, e);
					free(e);
					j++;
				}
			}
			break;
		}
		default:
			break;
	}
	return coverage;
}

void caryll_read_otl_lookup(font_file_pointer data, uint32_t tableLength, otl_lookup *lookup) {
	lookup->subtableCount = caryll_blt16u(data + lookup->_offset + 4);
	if (tableLength < lookup->_offset + 6 + 2 * lookup->subtableCount) {
		lookup->subtableCount = 0;
		lookup->subtables = NULL;
		return;
	}
	NEW_N(lookup->subtables, lookup->subtableCount);
	switch (lookup->type) {
		case otl_type_gsub_single:
			caryll_read_gsub_single(data, tableLength, lookup);
			break;
		default:
			lookup->type = otl_type_unknown;
			if (lookup->subtables) free(lookup->subtables);
			lookup->subtables = NULL;
			break;
	}
}

table_otl *caryll_read_otl(caryll_packet packet, uint32_t tag) {
	table_otl *otl = NULL;
	FOR_TABLE(tag, table) {
		font_file_pointer data = table.data;
		uint32_t length = table.length;
		otl = caryll_read_otl_common(
		    data, length,
		    (tag == 'GSUB' ? otl_type_gsub_unknown : tag == 'GPOS' ? otl_type_gpos_unknown : otl_type_unknown));
		if (!otl) goto FAIL;
		for (uint16_t j = 0; j < otl->lookupCount; j++) {
			caryll_read_otl_lookup(data, length, otl->lookups[j]);
		}
		return otl;
	FAIL:
		if (otl) caryll_delete_otl(otl);
		otl = NULL;
	}
	return NULL;
}
json_value *caryll_coverage_to_json(otl_coverage *coverage) {
	json_value *a = json_array_new(coverage->numGlyphs);
	for (uint16_t j = 0; j < coverage->numGlyphs; j++) {
		json_array_push(a, json_string_new(coverage->glyphs[j].name));
	}
	return preserialize(a);
}
void caryll_lookup_to_json(otl_lookup *lookup, json_value *dump) {
	switch (lookup->type) {
		case otl_type_gsub_single:
			caryll_gsub_single_to_json(lookup, dump);
			break;
		default:
			break;
	}
}
void caryll_otl_to_json(table_otl *table, json_value *root, caryll_dump_options *dumpopts, const char *tag) {
	if (!table || !table->languages || !table->lookups || !table->features) return;
	json_value *otl = json_object_new(3);
	{
		// dump script list
		json_value *languages = json_object_new(table->languageCount);
		for (uint16_t j = 0; j < table->languageCount; j++) {
			json_value *language = json_object_new(5);
			if (table->languages[j].requiredFeature) {
				json_object_push(language, "requiredFeature",
				                 json_string_new(table->languages[j].requiredFeature->name));
			}
			json_value *features = json_array_new(table->languages[j].featureCount);
			for (uint16_t k = 0; k < table->languages[j].featureCount; k++)
				if (table->languages[j].features[k]) {
					json_array_push(features, json_string_new(table->languages[j].features[k]->name));
				}
			json_object_push(language, "features", features);
			json_object_push(languages, table->languages[j].name, language);
		}
		json_object_push(otl, "languages", languages);
	}
	{
		// dump feature list
		json_value *features = json_object_new(table->featureCount);
		for (uint16_t j = 0; j < table->featureCount; j++) {
			json_value *feature = json_array_new(table->features[j].lookupCount);
			for (uint16_t k = 0; k < table->features[j].lookupCount; k++)
				if (table->features[j].lookups[k]) {
					json_array_push(feature, json_string_new(table->features[j].lookups[k]->name));
				}
			json_object_push(features, table->features[j].name, feature);
		}
		json_object_push(otl, "features", features);
	}
	{
		// dump lookups
		json_value *lookups = json_object_new(table->lookupCount);
		for (uint16_t j = 0; j < table->lookupCount; j++) {
			json_value *lookup = json_object_new(5);
			caryll_lookup_to_json(table->lookups[j], lookup);
			json_object_push(lookups, table->lookups[j]->name, lookup);
		}
		json_object_push(otl, "lookups", lookups);
	}
	json_object_push(root, tag, otl);
}

otl_coverage *caryll_coverage_from_json(json_value *cov) {
	otl_coverage *c;
	NEW(c);
	c->numGlyphs = cov->u.array.length;
	NEW_N(c->glyphs, c->numGlyphs);
	for (uint16_t j = 0; j < c->numGlyphs; j++) {
		if (cov->u.array.values[j]->type == json_string) {
			c->glyphs[j].gid = 0;
			c->glyphs[j].name =
			    sdsnewlen(cov->u.array.values[j]->u.string.ptr, cov->u.array.values[j]->u.string.length);
		} else {
			c->glyphs[j].gid = 0;
			c->glyphs[j].name = NULL;
		}
	}
	return c;
}

typedef struct {
	char *name;
	otl_lookup *lookup;
	UT_hash_handle hh;
} lookup_hash;

typedef struct {
	char *name;
	otl_feature *feature;
	UT_hash_handle hh;
} feature_hash;

typedef struct {
	char *name;
	otl_language_system *script;
	UT_hash_handle hh;
} script_hash;

static INLINE void parse_json_lookup_type(char *lt, otl_lookup *(*fn)(json_value *lookup), json_value *lookup,
                                          char *lookupName, lookup_hash **lh) {
	json_value *type = json_obj_get_type(lookup, "type", json_string);
	if (type && strcmp(type->u.string.ptr, lt) == 0) {
		lookup_hash *item = NULL;
		HASH_FIND_STR(*lh, lookupName, item);
		if (!item) {
			otl_lookup *_lookup = fn(lookup);
			if (_lookup) {
				NEW(item);
				item->name = sdsnew(lookupName);
				item->lookup = _lookup;
				item->lookup->name = sdsdup(item->name);
				HASH_ADD_STR(*lh, name, item);
			}
		}
	}
}

table_otl *caryll_otl_from_json(json_value *root, caryll_dump_options *dumpopts, char *tag) {
	table_otl *otl = NULL;
	NEW(otl);
	json_value *table = json_obj_get_type(root, tag, json_object);
	if (!table) goto FAIL;
	json_value *languages = json_obj_get_type(table, "languages", json_object);
	json_value *features = json_obj_get_type(table, "features", json_object);
	json_value *lookups = json_obj_get_type(table, "lookups", json_object);
	if (!languages || !features || !lookups) goto FAIL;

	lookup_hash *lh = NULL;
	{
		// lookup
		for (uint32_t j = 0; j < lookups->u.object.length; j++) {
			if (lookups->u.object.values[j].value->type == json_object) {
				char *lookupName = lookups->u.object.values[j].name;
				json_value *lookup = lookups->u.object.values[j].value;
				parse_json_lookup_type("gsub_single", caryll_gsub_single_from_json, lookup, lookupName, &lh);
			}
		}
	}
	feature_hash *fh = NULL;
	{
		// feature
		for (uint32_t j = 0; j < features->u.object.length; j++) {
			char *featureName = features->u.object.values[j].name;
			json_value *_feature = features->u.object.values[j].value;
			if (_feature->type == json_array) {
				uint16_t nal = 0;
				otl_lookup **al;
				NEW_N(al, _feature->u.array.length);
				for (uint16_t k = 0; k < _feature->u.array.length; k++) {
					json_value *term = _feature->u.array.values[k];
					if (term->type == json_string) {
						lookup_hash *item = NULL;
						HASH_FIND_STR(lh, term->u.string.ptr, item);
						if (item) { al[nal++] = item->lookup; }
					}
				}
				if (nal > 0) {
					feature_hash *s = NULL;
					HASH_FIND_STR(fh, featureName, s);
					if (!s) {
						NEW(s);
						s->name = sdsnew(featureName);
						NEW(s->feature);
						s->feature->name = sdsdup(s->name);
						s->feature->lookupCount = nal;
						s->feature->lookups = al;
						HASH_ADD_STR(fh, name, s);
					} else {
						free(al);
					}
				} else {
					FREE(al);
				}
			}
		}
	}
	script_hash *sh = NULL;
	{
		// languages
		for (uint32_t j = 0; j < languages->u.object.length; j++) {
			char *languageName = languages->u.object.values[j].name;
			json_value *_language = languages->u.object.values[j].value;
			if (_language->type == json_object) {
				otl_feature *requiredFeature = NULL;
				json_value *_rf = json_obj_get_type(_language, "requiredFeature", json_string);
				if (_rf) {
					// required feature term
					feature_hash *rf = NULL;
					HASH_FIND_STR(fh, _rf->u.string.ptr, rf);
					if (rf) { requiredFeature = rf->feature; }
				}

				uint16_t naf = 0;
				otl_feature **af = NULL;
				json_value *_features = json_obj_get_type(_language, "features", json_array);
				if (_features) {
					NEW_N(af, _features->u.array.length);
					for (uint16_t k = 0; k < _features->u.array.length; k++) {
						json_value *term = _features->u.array.values[k];
						if (term->type == json_string) {
							feature_hash *item = NULL;
							HASH_FIND_STR(fh, term->u.string.ptr, item);
							if (item) { af[naf++] = item->feature; }
						}
					}
				}
				if (requiredFeature || (af && naf > 0)) {
					script_hash *s = NULL;
					HASH_FIND_STR(sh, languageName, s);
					if (!s) {
						NEW(s);
						s->name = sdsnew(languageName);
						NEW(s->script);
						s->script->name = sdsdup(s->name);
						s->script->requiredFeature = requiredFeature;
						s->script->featureCount = naf;
						s->script->features = af;
						HASH_ADD_STR(sh, name, s);
					} else {
						if (af) { FREE(af); }
					}
				} else {
					if (af) { FREE(af); }
				}
			}
		}
	}
	{
		lookup_hash *s, *tmp;
		otl->lookupCount = HASH_COUNT(lh);
		NEW_N(otl->lookups, otl->lookupCount);
		uint16_t j = 0;
		HASH_ITER(hh, lh, s, tmp) {
			otl->lookups[j] = s->lookup;
			HASH_DEL(lh, s);
			sdsfree(s->name);
			free(s);
			j++;
		}
	}
	{
		feature_hash *s, *tmp;
		otl->featureCount = HASH_COUNT(fh);
		NEW_N(otl->features, otl->featureCount);
		uint16_t j = 0;
		HASH_ITER(hh, fh, s, tmp) {
			otl->features[j] = *s->feature;
			HASH_DEL(fh, s);
			sdsfree(s->name);
			free(s->feature);
			free(s);
			j++;
		}
	}
	{
		script_hash *s, *tmp;
		otl->languageCount = HASH_COUNT(sh);
		NEW_N(otl->languages, otl->languageCount);
		uint16_t j = 0;
		HASH_ITER(hh, sh, s, tmp) {
			otl->languages[j] = *s->script;
			HASH_DEL(sh, s);
			sdsfree(s->name);
			free(s->script);
			free(s);
			j++;
		}
	}
	return otl;
FAIL:
	caryll_delete_otl(otl);
	return NULL;
}
