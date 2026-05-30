// Usage:
// 1. Add PView to your layout string
// 2. Optionally define [svd] section in global or local config with following
//    optional variables:
//    - paths : Colon-separated list of directories to scan for .svd and .xml
//    files (e.g., paths=dir1:dir2).
//    - projectfiles : Colon-separated list of specific .svd or .xml files to
//    load automatically on startup.
//
// Keyboard Shortcuts:
// File Browser:
//   - Up/Down/Home/End : Navigate the file list.
//   - Enter : Load the highlighted SVD/XML file.
//   - Ctrl + C (without modifiers) : Copy the selected file name to the
//   clipboard.
//   - Textbox (Bottom) : Type a direct file path and hit Enter to load.
//
// Peripheral Viewer:
//   - Up/Down/Home/End : Navigate the hardware tree.
//   - Left/Right or Enter : Expand or collapse peripherals, clusters, and
//   registers.
//   - Page Up / Page Down : Jump to the previous or next node of a different
//   type
//   - Ctrl + P : Jump to the parent node of the currently selected item.
//   - Ctrl + Shift + D : Open a dedicated view to read the description of the
//   selected item (Press Esc to close).
//   - Ctrl + C : Copy the current value of the selected item to the clipboard.
//   - Ctrl + R : Clear the current viewer tree and return to the file browser.
//   - Ctrl + A : Return to the file browser (without clearing the current
//   tree).
//
// TODO : Enums, better descriptions, configurable formatting, watchpoints
#include <libxml/parser.h>
#include <libxml/tree.h>

template <class V> struct MapShortStr {
	struct {
		char *key;
		V value;
	} *array;
	size_t used, capacity;

	V *At(char *key, bool createIfNeeded) {
		if (used + 1 > capacity / 2) {
			MapShortStr grow = {};
			grow.capacity = capacity ? (capacity + 1) * 2 - 1 : 15;
			*(void **)&grow.array = calloc(grow.capacity, sizeof(array[0]));
			for (uintptr_t i = 0; i < capacity; i++)
				if (array[i].key)
					grow.Put(array[i].key, array[i].value);
			free(array);
			*this = grow;
		}

		uintptr_t slot = Hash((const uint8_t *)key, strlen(key)) % capacity;
		while (array[slot].key && strcmp(array[slot].key, key))
			slot = (slot + 1) % capacity;

		if (!array[slot].key && createIfNeeded) {
			used++;
			array[slot].key = key;
		}

		return &array[slot].value;
	}

	bool Has(char *key) {
		if (!capacity)
			return false;
		uintptr_t slot = Hash((uint8_t *)key, strlen(key)) % capacity;
		while (array[slot].key && strcmp(array[slot].key, key))
			slot = (slot + 1) % capacity;
		return array[slot].key;
	}

	V Get(char *key) { return *At(key, false); }
	void Put(char *key, V value) { *At(key, true) = value; }
	void Free() {
		free(array);
		array = nullptr;
		used = capacity = 0;
	}
};

struct Block {
	Block *next;
	char data[65536 - sizeof(Block *)];
};

struct Arena {
	Block *head;
	char *curr;
};

enum PWatchKind {
	PWATCH_PERIPHERAL,
	PWATCH_CLUSTER,
	PWATCH_REGISTER,
	PWATCH_BITS,
};

struct PWatch {
	Array<PWatch *> children; // periphs > regs > bits
	char *value;
	char *format;
	uint64_t addr;
	char *key;
	char *addedInfo;
	const char *description;
	// for bits
	uint64_t mask;
	uint64_t shift;
	uint64_t offset;
	int depth;
	int updateIndex;
	PWatchKind kind;
	int dimInfoIdx;
	int derivedInfoIdx;
	PWatch *parent;
	bool open;
};

struct PWatchWindowBase {
	UIElement e;
	Arena windowArena;
	int selectedRow;
	int scrollPos;
};

struct PWatchViewerWindow {
	PWatchWindowBase base;
	Array<PWatch *> rows;
	int updateIndex;
};

struct PWatchSVDEntry {
	char *name;
	char *source;
};

struct PWatchFileWindow {
	PWatchWindowBase base;
	Array<PWatchSVDEntry> rows;
	UITextbox *textbox;
};

struct DerivedFromInfo {
	char *symbolFullName;
	char *derivedFullName;
};

struct DimmedInfo {
	int dim;
	int increment;
	char *dimIndexStr;
};

struct SVDConfig {
	Array<char *> paths;
	Array<char *> projectFiles;
	Array<char *> configBuffers;
};

struct SVDContext {
	MapShortStr<PWatch *> symTab;
	Array<PWatch *> unresolved;
	Array<DerivedFromInfo> derivedInfo;
	Array<PWatch *> dimmed;
	Array<DimmedInfo> dimInfo;
	Arena temporaryStringArena;
	PWatchViewerWindow *w;
	Array<char> namesSb;
};

struct DimNameIterator {
	int currIdx;
	char *curr;
	char *dimIndexStr;
	int rangeEnd;
	bool isRange;
	bool isNumeric;
};

UIPanel *pluginPanel;
PWatchFileWindow *fileWindow;
PWatchViewerWindow *viewerWindow;

const char *pwatchPythonCode = R"(py
def gf_read_word(address,size):
    try:
        mem = gdb.selected_inferior().read_memory(address,size)
        val = int.from_bytes(mem, 'little')
        print(val)
    except:
        print("??")
end
)";

char regFormat[] = "0x%X";
char bitsFormat[] = "0b%b";

const char *emptyDescr = "No description available";

/////////////////////////////////////////////////////
// Utilities:
/////////////////////////////////////////////////////

char *ArenaStrdup(Arena *arena, const char *str) {
	size_t len = strlen(str) + 1;
	if (0 == arena->head || (arena->curr - arena->head->data) + len > sizeof(Block::data)) {
		Block *newBlock = (Block *)calloc(1, sizeof(Block));
		newBlock->next = arena->head;
		arena->head = newBlock;
		arena->curr = newBlock->data;
	}
	char *ptr = arena->curr;

	memcpy(ptr, str, len);
	arena->curr += len;

	return ptr;
}

void *ArenaAlloc(Arena *arena, size_t size, size_t alignment) {
	if (0 == alignment || alignment & (alignment - 1) || 0 == size || size > sizeof(Block::data)) {
		return nullptr;
	}
	uintptr_t currCast = (uintptr_t)arena->curr;
	size_t padding = (alignment - (currCast & (alignment - 1))) & (alignment - 1);

	if (0 == arena->head || size + padding + (arena->curr - arena->head->data) > sizeof(Block::data)) {
		Block *newBlock = (Block *)calloc(1, sizeof(Block));
		newBlock->next = arena->head;
		arena->head = newBlock;
		arena->curr = newBlock->data;
		// recalculate the padding
		currCast = (uintptr_t)arena->curr;
		padding = (alignment - (currCast & (alignment - 1))) & (alignment - 1);
	}
	void *ptr = arena->curr + alignment;
	arena->curr += alignment + size;
	return ptr;
}

void ArenaFree(Arena *arena) {
	while (arena->head) {
		Block *next = arena->head->next;
		free(arena->head);
		arena->head = next;
	}
	arena->curr = nullptr;
}

/////////////////////////////////////////////////////
// Parser:
/////////////////////////////////////////////////////

void PWatchChildrenCopy(SVDContext *context, PWatch *to, PWatch *from, bool isDerive) {
	if (!from->children.Length()) {
		return;
	}

	int childrenCount = from->children.Length();
	PWatch **childrenPtrs = (PWatch **)calloc(childrenCount, sizeof(PWatch *));
	PWatch *children =
		(PWatch *)ArenaAlloc(&context->w->base.windowArena, sizeof(PWatch) * childrenCount, alignof(PWatch));
	int frameCurr = context->namesSb.Length();
	for (int i = 0; i < childrenCount; i++, context->namesSb.length = frameCurr) {

		PWatch *childToCopy = from->children[i];
		PWatch *childCopyTo = &children[i];

		memcpy(childCopyTo, childToCopy, sizeof(PWatch));

		if (childToCopy->kind != PWATCH_CLUSTER) {
			childCopyTo->value = strdup("??");
		}
		context->namesSb.AddMany(childToCopy->key, strlen(childToCopy->key) + 1);

		if (isDerive) {
			char *fullyQualifiedName = ArenaStrdup(&context->temporaryStringArena, context->namesSb.array);
			if (childToCopy->derivedInfoIdx) {
				char *toCopy = context->derivedInfo[childToCopy->derivedInfoIdx].derivedFullName;
				context->derivedInfo.Add({
					.symbolFullName = fullyQualifiedName,
					.derivedFullName = toCopy,
				});
				childCopyTo->derivedInfoIdx = context->derivedInfo.Length() - 1;
			} else {
				context->symTab.Put(fullyQualifiedName, childCopyTo);
			}
			context->namesSb[context->namesSb.Length() - 1] = '.';
		}

		if (childToCopy->dimInfoIdx) {
			context->dimmed.Add(childCopyTo);
		}

		childrenPtrs[i] = childCopyTo;
		childCopyTo->parent = to;
		childCopyTo->addr = childToCopy->offset + to->addr;

		PWatchChildrenCopy(context, childCopyTo, childToCopy, isDerive);
	}
	to->children.array = childrenPtrs;
	to->children.allocated = childrenCount;
	to->children.length = childrenCount;
}

void DerivedResolve(SVDContext *context) {
	while (context->unresolved.Length() > 0) {
		int resolved_this_pass = 0;
		for (int i = 0; i < context->unresolved.Length(); i++) {
			PWatch *target = context->unresolved[i];
			DerivedFromInfo *targetInfo = &context->derivedInfo[target->derivedInfoIdx];
			PWatch *clone = *context->symTab.At(targetInfo->derivedFullName, false);
			if (clone) {

				if (clone->derivedInfoIdx) {
					continue;
				}

				target->derivedInfoIdx = 0;

				context->symTab.Put(targetInfo->symbolFullName, target);
				context->namesSb.AddMany(targetInfo->symbolFullName, strlen(targetInfo->symbolFullName) + 1);

				if (clone->dimInfoIdx) {
					if (target->dimInfoIdx) {
						DimmedInfo *copy_from = &context->dimInfo[clone->dimInfoIdx];
						DimmedInfo *copy_to = &context->dimInfo[target->dimInfoIdx];

						if (!copy_to->dim)
							copy_to->dim = copy_from->dim;
						if (!copy_to->increment)
							copy_to->increment = copy_from->increment;
						if (!copy_to->dimIndexStr)
							copy_to->dimIndexStr = copy_from->dimIndexStr;

					} else {
						context->dimmed.Add(target);
					}
				}

				if (!target->mask)
					target->mask = clone->mask;
				if (!target->shift)
					target->shift = clone->shift;
				if (target->description == emptyDescr)
					target->description = clone->description;

				if (target->children.Length() == 0) {
					context->namesSb[context->namesSb.Length() - 1] = '.';
					PWatchChildrenCopy(context, target, clone, true);
				}

				context->unresolved.DeleteSwap(i);

				context->namesSb.length = 0;
				resolved_this_pass++;
			}
		}
		if (resolved_this_pass == 0) {
			for (int i = 0; i < context->unresolved.Length(); i++) {
				PWatch *failed = context->unresolved[i];
				DerivedFromInfo *failedInfo = &context->derivedInfo[failed->derivedInfoIdx];
				fprintf(stderr,
						"Failed derivedFrom resolution "
						"name=%s,derivedFrom=%s",
						failed->key, failedInfo->derivedFullName);
				return;
			}
		}
	}
}

void DimNameIteratorInit(DimNameIterator *it, char *dimIndexStr) {
	if (!dimIndexStr) {
		it->currIdx = 0;
		it->isNumeric = true;
	} else if (strchr(dimIndexStr, ',')) {
		it->dimIndexStr = dimIndexStr;
		it->curr = dimIndexStr;
		it->isNumeric = true; // to produce indeces on failure
	} else if (char *delim = strchr(dimIndexStr, '-')) {
		if (delim[1] > '9') {
			it->currIdx = (int)dimIndexStr[0];
			it->rangeEnd = (int)delim[1];
			it->isNumeric = false;
		} else {
			it->currIdx = atoi(dimIndexStr);
			it->rangeEnd = atoi(delim + 1);
			it->isNumeric = true;
		}
		it->isRange = true;
	} else {
		fprintf(stderr,
				"unrecoginsed dim index str %s, falling back to 0-N "
				"array indeces",
				dimIndexStr);
		it->currIdx = 0;
		it->isNumeric = true;
	}
}
void DimNameIteratorNext(DimNameIterator *it, char *buff, int buffLen) {
	if (!it->curr) {
		if (it->isRange && it->rangeEnd < it->currIdx) {
			fprintf(stderr, "dim range too short continuing with next index %d", it->currIdx);
		}
		if (it->dimIndexStr) {
			fprintf(stderr,
					"coma separated list too short continuing with "
					"index %d",
					it->currIdx);
		}
		StringFormat(buff, buffLen, it->isNumeric == true ? "%lu" : "%c", it->currIdx);
	} else {
		char *delim = strchr(it->curr, ',');

		if (delim) {
			int len = (int)(delim - it->curr);
			StringFormat(buff, buffLen, "%.*s", len, it->curr);
			it->curr = delim + 1;
		} else {
			StringFormat(buff, buffLen, "%s", it->curr);
			it->curr = nullptr;
		}
	}

	it->currIdx++;
}

void DimsResolve(SVDContext *context) {

	char formatBuff[128];
	char token[64];
	for (int i = 0; i < context->dimmed.Length(); i++) {
		PWatch *expand = context->dimmed[i];
		DimmedInfo *info = &context->dimInfo[expand->dimInfoIdx];
		char *format = expand->key;
		DimNameIterator it = {0};
		DimNameIteratorInit(&it, info->dimIndexStr);
		DimNameIteratorNext(&it, token, 64);
		StringFormat(formatBuff, 128, format, token);
		expand->key = ArenaStrdup(&context->w->base.windowArena, formatBuff);

		if (info->dim <= 1) {
			continue;
		}

		int dims = info->dim;
		int currIncrement = info->increment;
		int dimIncrement = info->increment;
		PWatch *dimmed =
			(PWatch *)ArenaAlloc(&context->w->base.windowArena, sizeof(PWatch) * dims - 1, alignof(PWatch));
		PWatch **dimmedPtrs = (PWatch **)calloc(dims - 1, sizeof(PWatch *));

		expand->dimInfoIdx = 0;
		for (int i = 1; i < dims; i++, currIncrement += dimIncrement) {

			DimNameIteratorNext(&it, token, 64);
			StringFormat(formatBuff, 128, format, token);
			char *name = ArenaStrdup(&context->w->base.windowArena, formatBuff);

			PWatch *currDimmed = &dimmed[i - 1];
			dimmedPtrs[i - 1] = currDimmed;

			memcpy(currDimmed, expand, sizeof(PWatch));
			currDimmed->key = name;

			if (expand->kind == PWATCH_BITS) {
				currDimmed->shift += currIncrement;
				currDimmed->mask <<= currIncrement;
				currDimmed->value = strdup("??");

				StringFormat(formatBuff, 128, "[%lu:%lu]", currDimmed->shift + __builtin_popcount(currDimmed->mask) - 1,
							 currDimmed->shift);
				currDimmed->addedInfo = ArenaStrdup(&context->w->base.windowArena, formatBuff);
			} else if (expand->kind == PWATCH_PERIPHERAL) {
				currDimmed->addr += currIncrement;
				StringFormat(formatBuff, 128, "%#X", currDimmed->addr);
				currDimmed->value = ArenaStrdup(&context->w->base.windowArena, formatBuff);
				PWatchChildrenCopy(context, currDimmed, expand, false);
			} else {
				currDimmed->offset += currIncrement;
				StringFormat(formatBuff, 128, "%#X", currDimmed->offset);
				char *offsetStr = ArenaStrdup(&context->w->base.windowArena, formatBuff);
				if (expand->kind > PWATCH_CLUSTER) {
					currDimmed->value = strdup("??");
					currDimmed->addedInfo = offsetStr;
				} else {
					currDimmed->value = offsetStr;
				}

				currDimmed->addr += currIncrement;
				PWatchChildrenCopy(context, currDimmed, expand, false);
			}
		}

		uintptr_t index = 0;
		if (expand->parent) {
			expand->parent->children.Contains(expand, &index);
			expand->parent->children.InsertMany(dimmedPtrs, index + 1, dims - 1);
		} else {
			context->w->rows.Contains(expand, &index);
			context->w->rows.InsertMany(dimmedPtrs, index + 1, dims - 1);
		}
		free(dimmedPtrs);
	}
}

void FieldsParse(SVDContext *context, xmlNodePtr fieldsNode, PWatch *reg, int depth) {
	int frameCurr = context->namesSb.Length();
	for (xmlNodePtr it = fieldsNode->children; it; it = it->next) {

		if (it->type != XML_ELEMENT_NODE || xmlStrcmp(it->name, BAD_CAST "field")) {
			continue;
		}

		char *xmlName = 0;
		const char *description = emptyDescr;
		int bitInfoType = 0;
		char *bitInfoStr1 = 0;
		char *bitInfoStr2 = 0;

		int dims = 0;
		char *DimIndexStr = 0;
		int dimIncrement = 0;

		for (xmlNodePtr childIt = it->children; childIt; childIt = childIt->next) {
			if (childIt->type != XML_ELEMENT_NODE) {
				continue;
			}

			if (0 == xmlStrcmp(childIt->name, BAD_CAST "name")) {
				xmlName = (char *)xmlNodeGetContent(childIt);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "description")) {
				char *xmlDescriptionStr = (char *)xmlNodeGetContent(childIt);
				description = ArenaStrdup(&context->w->base.windowArena, xmlDescriptionStr);
				xmlFree(xmlDescriptionStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "bitOffset")) {
				char *xmlBitInfoStr = (char *)xmlNodeGetContent(childIt);
				bitInfoStr1 = ArenaStrdup(&context->temporaryStringArena, xmlBitInfoStr);
				xmlFree(xmlBitInfoStr);
				bitInfoType |= 1;
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "bitWidth")) {
				char *xmlBitInfoStr = (char *)xmlNodeGetContent(childIt);
				bitInfoStr2 = ArenaStrdup(&context->temporaryStringArena, xmlBitInfoStr);
				xmlFree(xmlBitInfoStr);
				bitInfoType |= 2;
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "msb")) {
				char *xmlBitInfoStr = (char *)xmlNodeGetContent(childIt);
				bitInfoStr1 = ArenaStrdup(&context->temporaryStringArena, xmlBitInfoStr);
				xmlFree(xmlBitInfoStr);
				bitInfoType |= 4;
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "lsb")) {
				char *xmlBitInfoStr = (char *)xmlNodeGetContent(childIt);
				bitInfoStr2 = ArenaStrdup(&context->temporaryStringArena, xmlBitInfoStr);
				xmlFree(xmlBitInfoStr);
				bitInfoType |= 8;
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "bitRange")) {
				char *xmlBitInfoStr = (char *)xmlNodeGetContent(childIt);
				bitInfoStr1 = ArenaStrdup(&context->temporaryStringArena, xmlBitInfoStr);
				xmlFree(xmlBitInfoStr);
				bitInfoType |= 16;
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dim")) {
				char *dimStr = (char *)xmlNodeGetContent(childIt);
				dims = atoi(dimStr);
				xmlFree(dimStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dimIndex")) {
				char *xmlDimIdxStr = (char *)xmlNodeGetContent(childIt);
				DimIndexStr = ArenaStrdup(&context->temporaryStringArena, xmlDimIdxStr);
				xmlFree(xmlDimIdxStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dimIncrement")) {
				char *dimIncStr = (char *)xmlNodeGetContent(childIt);
				dimIncrement = atoi(dimIncStr);
				xmlFree(dimIncStr);
			}
		}

		if (0 == xmlName) {
			fprintf(stderr,
					"Malformed field, no field name provided in "
					"register %s\n",
					reg->key);
			continue;
		}

		if (!it->properties && !bitInfoType) {
			fprintf(stderr,
					"Malformed concrete field %s , no bit info "
					"provided in register %s falling back to full "
					"register reports\n",
					xmlName, reg->key);
		}

		if (!dims && (DimIndexStr || dimIncrement)) {
			fprintf(stderr,
					"Malformed dimmed field %s , no dim tag provided "
					"in register %s interpreting as a normal field\n",
					xmlName, reg->key);
		}

		char *name = 0;

		PWatch *field = (PWatch *)ArenaAlloc(&context->w->base.windowArena, sizeof(PWatch), alignof(PWatch));

		if (dims) {
			name = ArenaStrdup(&context->temporaryStringArena, xmlName);
			context->dimInfo.Add({.dim = dims, .increment = dimIncrement, .dimIndexStr = DimIndexStr});
			context->dimmed.Add(field);
			field->dimInfoIdx = context->dimInfo.Length() - 1;
		} else {
			name = ArenaStrdup(&context->w->base.windowArena, xmlName);
		}

		xmlFree(xmlName);

		uint64_t lsb = 0;
		uint64_t msb = 0;

		if (bitInfoType == 1) {
			lsb = msb = atoi(bitInfoStr1);
		} else if (bitInfoType == 3) {
			lsb = atoi(bitInfoStr1);
			msb = atoi(bitInfoStr2) + lsb - 1;
		} else if (bitInfoType == 12) {
			msb = atoi(bitInfoStr1);
			lsb = atoi(bitInfoStr2);
		} else if (bitInfoType == 16) {
			char *delim = strchr(bitInfoStr1, ':');
			msb = atoi(bitInfoStr1 + 1);
			lsb = atoi(delim + 1);
		} else if (bitInfoType > 0) {
			// Malformed bit info, fallback to full register
			msb = 31;
			lsb = 0;
		}

		if (bitInfoType) {
			char formatbuff[64];
			StringFormat(formatbuff, 64, "[%lu:%lu]", msb, lsb);
			field->addedInfo = ArenaStrdup(&context->w->base.windowArena, formatbuff);

			field->mask = (msb - lsb == 63) ? ~0ULL : (1ULL << (msb - lsb + 1)) - 1;
			;
			field->shift = lsb;
		}

		context->namesSb.AddMany(name, strlen(name) + 1);
		char *fullyQualifiedName = ArenaStrdup(&context->temporaryStringArena, context->namesSb.array);

		field->key = name;
		field->value = strdup("??");
		field->addr = reg->addr;
		field->kind = PWATCH_BITS;
		field->parent = reg;
		field->depth = depth;
		field->format = bitsFormat;
		field->description = description;

		if (it->properties) {
			char *derivedFullName = 0;
			char *attrValue = (char *)xmlGetProp(it, BAD_CAST "derivedFrom");
			if (!strchr(attrValue, '.')) {
				context->namesSb.length = frameCurr;
				context->namesSb.AddMany(attrValue, strlen(attrValue) + 1);
				derivedFullName = ArenaStrdup(&context->temporaryStringArena, context->namesSb.array);
			} else {
				derivedFullName = ArenaStrdup(&context->temporaryStringArena, attrValue);
			}
			context->derivedInfo.Add({
				.symbolFullName = fullyQualifiedName,
				.derivedFullName = derivedFullName,
			});
			field->derivedInfoIdx = context->derivedInfo.Length() - 1;
			xmlFree(attrValue);
		} else {
			context->symTab.Put(fullyQualifiedName, field);
		}

		reg->children.Add(field);
		context->namesSb.length = frameCurr;
	}
}

void RegistersOrClustersParse(SVDContext *context, xmlNodePtr registersNode, PWatch *parent, int depth) {

	int frameCurr = context->namesSb.Length();

	for (xmlNodePtr it = registersNode->children; it; it = it->next) {
		if (it->type != XML_ELEMENT_NODE) {
			continue;
		}

		int type = 0;

		if (0 == xmlStrcmp(it->name, BAD_CAST "register")) {
			type = 1;
		} else if (0 == xmlStrcmp(it->name, BAD_CAST "cluster")) {
			type = 2;
		} else {
			continue;
		}

		char *xmlName = 0;
		const char *description = emptyDescr;
		char *adressOffsetStr = 0;

		xmlNodePtr fields = 0;

		int dims = 0;
		int dimIncrement = 0;
		char *DimIndexStr = 0;

		for (xmlNodePtr childIt = it->children; childIt; childIt = childIt->next) {
			if (childIt->type != XML_ELEMENT_NODE) {
				continue;
			}

			if (0 == xmlStrcmp(childIt->name, BAD_CAST "name")) {
				xmlName = (char *)xmlNodeGetContent(childIt);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "description")) {
				char *xmlDescriptionStr = (char *)xmlNodeGetContent(childIt);
				description = ArenaStrdup(&context->w->base.windowArena, xmlDescriptionStr);
				xmlFree(xmlDescriptionStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "addressOffset")) {
				char *xmlAddressOffset = (char *)xmlNodeGetContent(childIt);
				adressOffsetStr = ArenaStrdup(&context->w->base.windowArena, xmlAddressOffset);
				xmlFree(xmlAddressOffset);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dim")) {
				char *dimStr = (char *)xmlNodeGetContent(childIt);
				dims = atoi(dimStr);
				xmlFree(dimStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dimIndex")) {
				char *xmlDimIdxStr = (char *)xmlNodeGetContent(childIt);
				DimIndexStr = ArenaStrdup(&context->temporaryStringArena, xmlDimIdxStr);
				xmlFree(xmlDimIdxStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dimIncrement")) {
				char *dimIncStr = (char *)xmlNodeGetContent(childIt);
				dimIncrement = strtoul(dimIncStr, 0, 16);
				xmlFree(dimIncStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "fields")) {
				fields = childIt;
			}
		}

		if (0 == xmlName) {
			fprintf(stderr, "Malformed %s, no name provided in parent %s\n", type == 1 ? "register" : "cluster",
					parent->key);
			continue;
		}

		if (!adressOffsetStr) {
			fprintf(stderr, "Malformed %s %s, no addressOffset in parent %s\n", type == 1 ? "register" : "cluster",
					xmlName, parent->key);
			xmlFree(xmlName);
			continue;
		}

		if (!dims && (DimIndexStr || dimIncrement)) {
			const char *typeStr = type == 1 ? "register" : "cluster";
			fprintf(stderr,
					"Malformed dimmed %s %s , no dim tag provided "
					"in parent %s interpreting as a normal %s\n",
					typeStr, xmlName, parent->key, typeStr);
		}

		char *name = 0;

		PWatch *obj = (PWatch *)ArenaAlloc(&context->w->base.windowArena, sizeof(PWatch), alignof(PWatch));

		if (dims) {
			name = ArenaStrdup(&context->temporaryStringArena, xmlName);
			context->dimInfo.Add({.dim = dims, .increment = dimIncrement, .dimIndexStr = DimIndexStr});
			context->dimmed.Add(obj);
			obj->dimInfoIdx = context->dimInfo.Length() - 1;
		} else {
			name = ArenaStrdup(&context->w->base.windowArena, xmlName);
		}

		xmlFree(xmlName);

		obj->parent = parent;
		obj->depth = depth;
		obj->key = name;
		obj->addedInfo = adressOffsetStr;
		obj->offset = strtoul(adressOffsetStr, 0, 16);
		obj->addr = obj->parent->addr + obj->offset;
		obj->description = description;

		context->namesSb.AddMany(name, strlen(name) + 1);
		char *fullyQualifiedName = ArenaStrdup(&context->temporaryStringArena, context->namesSb.array);

		if (type == 1) {
			obj->value = strdup("??");
			obj->kind = PWATCH_REGISTER;
			obj->format = regFormat;
			if (fields) {
				context->namesSb[context->namesSb.Length() - 1] = '.';
				FieldsParse(context, fields, obj, depth + 1);
			}
		} else {
			obj->kind = PWATCH_CLUSTER;
			obj->value = obj->addedInfo;
			obj->addedInfo = nullptr;
			context->namesSb[context->namesSb.Length() - 1] = '.';
			RegistersOrClustersParse(context, it, obj, depth + 1);
		}

		if (it->properties) {

			char *attrValue = (char *)xmlGetProp(it, BAD_CAST "derivedFrom");
			char *derivedFullName = 0;
			if (!strchr(attrValue, '.')) {
				context->namesSb.length = frameCurr;
				context->namesSb.AddMany(attrValue, strlen(attrValue) + 1);
				derivedFullName = ArenaStrdup(&context->temporaryStringArena, context->namesSb.array);
			} else {
				derivedFullName = ArenaStrdup(&context->temporaryStringArena, attrValue);
			}
			context->derivedInfo.Add({
				.symbolFullName = fullyQualifiedName,
				.derivedFullName = derivedFullName,
			});

			obj->derivedInfoIdx = context->derivedInfo.Length() - 1;
			context->unresolved.Add(obj);
			xmlFree(attrValue);

		} else {
			context->symTab.Put(fullyQualifiedName, obj);
		}

		parent->children.Add(obj);
		context->namesSb.length = frameCurr;
	}
}

void PeripheralsParse(SVDContext *context, xmlNodePtr peripheralsNode) {

	int frameCurr = context->namesSb.Length();

	for (xmlNodePtr it = peripheralsNode->children; it; it = it->next, context->namesSb.length = frameCurr) {
		if (it->type != XML_ELEMENT_NODE || xmlStrcmp(it->name, BAD_CAST "peripheral")) {
			continue;
		}

		char *xmlName = 0;
		const char *description = emptyDescr;
		char *baseAddressStr = 0;
		xmlNodePtr regs = 0;

		int dims = 0;
		char *DimIndexStr = 0;
		int dimIncrement = 0;

		for (xmlNodePtr childIt = it->children; childIt; childIt = childIt->next) {
			if (childIt->type != XML_ELEMENT_NODE) {
				continue;
			}

			if (0 == xmlStrcmp(childIt->name, BAD_CAST "name")) {
				xmlName = (char *)xmlNodeGetContent(childIt);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "description")) {
				char *xmlDescriptionStr = (char *)xmlNodeGetContent(childIt);
				description = ArenaStrdup(&context->w->base.windowArena, xmlDescriptionStr);
				xmlFree(xmlDescriptionStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "baseAddress")) {
				char *xmlAddr = (char *)xmlNodeGetContent(childIt);
				baseAddressStr = ArenaStrdup(&context->w->base.windowArena, xmlAddr);
				xmlFree(xmlAddr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "registers")) {
				regs = childIt;
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dim")) {
				char *dimStr = (char *)xmlNodeGetContent(childIt);
				dims = atoi(dimStr);
				xmlFree(dimStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dimIndex")) {
				char *xmlDimIdxStr = (char *)xmlNodeGetContent(childIt);
				DimIndexStr = ArenaStrdup(&context->temporaryStringArena, xmlDimIdxStr);
				xmlFree(xmlDimIdxStr);
			} else if (0 == xmlStrcmp(childIt->name, BAD_CAST "dimIncrement")) {
				char *dimIncStr = (char *)xmlNodeGetContent(childIt);
				dimIncrement = atoi(dimIncStr);
				xmlFree(dimIncStr);
			}
		}

		if (0 == xmlName) {
			fprintf(stderr,
					"Malformed peripheral number %d, no name "
					"provided \n",
					context->w->rows.Length());
			continue;
		}

		if (!baseAddressStr) {
			fprintf(stderr, "Malformed peripheral %s no base address\n", xmlName);
			xmlFree(xmlName);
			continue;
		}

		if (!dims && (DimIndexStr || dimIncrement)) {
			fprintf(stderr,
					"Malformed dimmed peripheral %s , no dim tag "
					"provided interpreting as a normal peripheral\n",
					xmlName);
		}

		char *name = 0;
		PWatch *periph = (PWatch *)ArenaAlloc(&context->w->base.windowArena, sizeof(PWatch), alignof(PWatch));

		if (dims) {
			name = ArenaStrdup(&context->temporaryStringArena, xmlName);
			context->dimInfo.Add({.dim = dims, .increment = dimIncrement, .dimIndexStr = DimIndexStr});
			context->dimmed.Add(periph);
			periph->dimInfoIdx = context->dimInfo.Length() - 1;
		} else {
			name = ArenaStrdup(&context->w->base.windowArena, xmlName);
		}
		xmlFree(xmlName);
		context->namesSb.AddMany(name, strlen(name) + 1);
		char *fullyQualifiedName = ArenaStrdup(&context->temporaryStringArena, context->namesSb.array);

		periph->key = name;
		periph->value = baseAddressStr;
		periph->addr = strtoul(baseAddressStr, 0, 16);
		periph->description = description;

		if (regs) {
			context->namesSb[context->namesSb.Length() - 1] = '.';
			RegistersOrClustersParse(context, regs, periph, 1);
		}

		context->namesSb.length = frameCurr;

		if (it->properties) {

			char *attrValue = (char *)xmlGetProp(it, BAD_CAST "derivedFrom");
			char *derivedFullName = 0;
			derivedFullName = ArenaStrdup(&context->temporaryStringArena, attrValue);

			context->derivedInfo.Add({
				.symbolFullName = fullyQualifiedName,
				.derivedFullName = derivedFullName,
			});
			periph->derivedInfoIdx = context->derivedInfo.Length() - 1;
			context->unresolved.Add(periph);
			xmlFree(attrValue);

		} else {
			context->symTab.Put(fullyQualifiedName, periph);
		}

		context->w->rows.Add(periph);
		context->namesSb.length = frameCurr;
	}
}

/////////////////////////////////////////////////////
// UI:
/////////////////////////////////////////////////////

void PWatchEnsureRowVisible(PWatchWindowBase *w, int index, int rowLength) {
	if (w->selectedRow < 0)
		w->selectedRow = 0;
	else if (w->selectedRow >= rowLength)
		w->selectedRow = rowLength - 1;
	UIScrollBar *scroll = ((UIPanel *)w->e.parent)->scrollBar;
	int rowHeight = (int)(UI_SIZE_TEXTBOX_HEIGHT * w->e.window->scale);
	int start = index * rowHeight, end = (index + 1) * rowHeight, height = UI_RECT_HEIGHT(w->e.parent->bounds);
	bool unchanged = false;
	if (end >= scroll->position + height)
		scroll->position = end - height;
	else if (start <= scroll->position)
		scroll->position = start;
	else
		unchanged = true;
	if (!unchanged)
		UIElementRefresh(w->e.parent);
}

void WindowSwitch(PWatchWindowBase *target, int targetRowLength) {
	UIPanel *parentPanel = (UIPanel *)target->e.parent;
	PWatchWindowBase *prev = (PWatchWindowBase *)parentPanel->e.cp;
	prev->scrollPos = parentPanel->scrollBar->position;
	prev->e.flags |= UI_ELEMENT_HIDE;
	target->e.flags &= ~(UI_ELEMENT_HIDE);
	UIElementFocus(&target->e);
	UIElementRefresh(&parentPanel->e);
	UIElementRefresh(&target->e);
	parentPanel->e.cp = target;
	parentPanel->scrollBar->position = target->scrollPos;
	PWatchEnsureRowVisible(target, target->selectedRow, targetRowLength);
}

int PWatchKeyboardNavigation(PWatchWindowBase *w, intptr_t code, int rowLength) {

	int handeled = 1;

	if (code == UI_KEYCODE_UP) {
		if (w->e.window->shift) {
			if (currentLine > 1) {
				DisplaySetPosition(NULL, currentLine - 1, false);
			}
		} else {
			w->selectedRow--;
		}
	} else if (code == UI_KEYCODE_DOWN) {
		if (w->e.window->shift) {
			if (currentLine < displayCode->lineCount) {
				DisplaySetPosition(NULL, currentLine + 1, false);
			}
		} else {
			w->selectedRow++;
		}
	} else if (code == UI_KEYCODE_HOME) {
		w->selectedRow = 0;
	} else if (code == UI_KEYCODE_END) {
		w->selectedRow = rowLength - 1;
	} else {
		handeled = 0;
	}

	return handeled;
}

int PWatchViewerWindowPopulate(PWatchViewerWindow *w, char *filename) {
	char pathBuff[PATH_MAX];

	if (0 == realpath(filename, pathBuff)) {
		UIDialogShow(w->base.e.window, 0, "Could not load SVD file %s\n%f%B", filename, "OK");
		return 0;
	}

	xmlDocPtr doc = xmlReadFile(pathBuff, NULL, 0);
	xmlNodePtr root = xmlDocGetRootElement(doc);

	if (0 == root) {
		UIDialogShow(w->base.e.window, 0, "Could not find XML root in file %s\n%f%B", filename, "OK");
		return 0;
	}

	xmlNodePtr periphs = 0;

	for (xmlNodePtr it = root->children; it; it = it->next) {
		if (it->type != XML_ELEMENT_NODE || xmlStrcmp(it->name, BAD_CAST "peripherals")) {
			continue;
		}
		periphs = it;
		break;
	}

	if (0 == periphs) {
		UIDialogShow(w->base.e.window, 0, "Could not find peripherals XML node in file %s\n%f%B", filename, "OK");
		return 0;
	}

	SVDContext context =
		{
			.symTab =
				{
					.array = (typeof(context.symTab.array[0]) *)calloc(262144, sizeof(context.symTab.array[0])),
					.capacity = 262144,
				},
			.unresolved =
				{
					.array = (PWatch **)calloc(1024, sizeof(PWatch *)),
					.allocated = 1024,
				},
			.derivedInfo =
				{
					.array = (DerivedFromInfo *)calloc(1024, sizeof(DerivedFromInfo)),
					.length = 1,
					.allocated = 1024,
				},
			.dimmed =
				{
					.array = (PWatch **)calloc(1024, sizeof(PWatch *)),
					.allocated = 1024,
				},
			.dimInfo =
				{
					.array = (DimmedInfo *)calloc(1024, sizeof(DimmedInfo)),
					.length = 1,
					.allocated = 1024,
				},
			.w = w,
			.namesSb = {
				.array = (char *)calloc(128, sizeof(char)),
				.allocated = 128,
			}};

	PeripheralsParse(&context, periphs);
	DerivedResolve(&context);
	DimsResolve(&context);

	w->updateIndex++;

	xmlFreeDoc(doc);

	context.symTab.Free();
	context.unresolved.Free();
	ArenaFree(&context.temporaryStringArena);
	context.dimInfo.Free();
	context.dimmed.Free();
	context.derivedInfo.Free();
	context.namesSb.Free();

	return 1;
}

void PWatchFileWindowPopulate(PWatchFileWindow *w, const char *dirPath) {
	DIR *d;
	struct dirent *dir;

	d = opendir(dirPath);

	if (0 == d) {
		UIDialogShow(w->base.e.parent->window, 0, "Could not open directory %s\n%f%B", dirPath, "OK");
		return;
	}

	char *ownPathStr = ArenaStrdup(&w->base.windowArena, dirPath);
	while ((dir = readdir(d)) != NULL) {
		if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
			continue;
		}
		const char *dot = strrchr(dir->d_name, '.');
		if (dot && (0 == strcmp(dot, ".xml") || 0 == strcmp(dot, ".svd"))) {
			w->rows.Add({
				.name = ArenaStrdup(&w->base.windowArena, dir->d_name),
				.source = ownPathStr,
			});
		}
	}
	closedir(d);
}

void PWatchDestroyChildren(PWatch *obj) {
	if (obj->kind > PWATCH_CLUSTER) {
		free(obj->value);
	}

	if (obj->children.Length() > 0) {
		for (int i = 0; i < obj->children.Length(); i++) {
			PWatchDestroyChildren(obj->children[i]);
		}
		obj->children.Free();
	}
}

void PWatchViewerWindowClear(PWatchViewerWindow *w) {
	for (int i = 0; i < w->rows.Length(); i++) {
		if (w->rows[i]->kind == PWATCH_PERIPHERAL) {
			PWatchDestroyChildren(w->rows[i]);
		}
	}
	ArenaFree(&w->base.windowArena);
	w->rows.Free();
	w->base.selectedRow = 0;
}

void PWatchViewerInsertChildrenRows2(PWatchViewerWindow *w, PWatch *d, Array<PWatch *> *array) {
	for (int i = 0; i < d->children.Length(); i++) {
		array->Add(d->children[i]);
		if (d->kind != PWATCH_REGISTER && d->children[i]->open)
			PWatchViewerInsertChildrenRows2(w, d->children[i], array);
	}
}

void PWatchViewerInsertChildrenRows(PWatchViewerWindow *w, PWatch *d, int position, bool ensureLastVisible) {
	Array<PWatch *> array = {};
	PWatchViewerInsertChildrenRows2(w, d, &array);
	w->rows.InsertMany(array.array, position, array.Length());
	if (ensureLastVisible)
		PWatchEnsureRowVisible(&w->base, position + array.Length() - 1, w->rows.Length());
	array.Free();
}

int PWatchPanelMessage(UIElement *element, UIMessage message, int di, void *dp) {
	if (message == UI_MSG_LEFT_DOWN) {
		UIElement *child = ((UIElement *)element->cp);
		UIElementFocus(child);
		UIElementRepaint(child, nullptr);
	}

	return 0;
}

int PWatchViewerCodeMessage(UIElement *element, UIMessage message, int di, void *dp) {
	int result = 0;

	if (message == UI_MSG_KEY_TYPED) {
		UIKeyTyped *m = (UIKeyTyped *)dp;
		result = 1;
		if (m->code == UI_KEYCODE_ESCAPE) {
			PWatchViewerWindow *w = (PWatchViewerWindow *)element->cp;
			w->base.e.flags &= ~(UI_ELEMENT_HIDE);
			w->base.e.parent->flags &= ~(UI_PANEL_EXPAND);
			element->parent->cp = w;
			UIElementDestroy(element);

			((UIPanel *)w->base.e.parent)->scrollBar->position = w->base.scrollPos;
			PWatchEnsureRowVisible(&w->base, w->base.selectedRow, w->rows.Length());
			UIElementFocus(&w->base.e);
			UIElementRefresh(&w->base.e);
			UIElementRefresh(w->base.e.parent);
			w->updateIndex++;
		} else {
			result = 0;
		}
	}
	return result;
}

int PWatchFileTextBoxMessage(UIElement *element, UIMessage message, int di, void *dp) {
	int result = 0;
	UITextbox *textbox = (UITextbox *)element;
	if (message == UI_MSG_LEFT_DOWN) {
		UIElementRefresh(element->parent);
	} else if (message == UI_MSG_KEY_TYPED) {
		UIKeyTyped *m = (UIKeyTyped *)dp;
		result = 1;
		if (m->code == UI_KEYCODE_ENTER && textbox->bytes && !element->window->shift) {
			char buffer[1024];
			StringFormat(buffer, 1024, "%.*s", (int)textbox->bytes, textbox->string);

			if (PWatchViewerWindowPopulate(viewerWindow, buffer)) {
				UITextboxClear(textbox, false);
				WindowSwitch(&viewerWindow->base, viewerWindow->rows.Length());
			}

		} else if (m->code == UI_KEYCODE_UP) {
			PWatchFileWindow *w = (PWatchFileWindow *)element->cp;
			w->base.selectedRow = w->rows.Length() - 1;
			UIElementFocus(&w->base.e);
			UIElementRefresh(&w->base.e);
		} else {
			result = 0;
		}
	}
	return result;
}

void PWatchFileWindowSpawnTextbox(PWatchFileWindow *w) {
	int rowHeight = (int)(UI_SIZE_TEXTBOX_HEIGHT * w->base.e.window->scale);
	UIRectangle row = w->base.e.bounds;
	row.t += w->rows.Length() * rowHeight, row.b = row.t + rowHeight;
	w->textbox = UITextboxCreate(&w->base.e, UI_ELEMENT_H_FILL);
	w->textbox->e.messageUser = PWatchFileTextBoxMessage;
	w->textbox->e.cp = w;
	UIElementRelayout(&w->base.e);
	UIElementMove(&w->textbox->e, row, true);
	UIElementFocus(&w->textbox->e);
}

void PWatchViewerSpawnDescription(PWatchViewerWindow *w) {
	const char *displayedDescr = w->rows[w->base.selectedRow]->description;
	w->base.scrollPos = ((UIPanel *)w->base.e.parent)->scrollBar->position;
	w->base.e.flags |= UI_ELEMENT_HIDE;

	// TODO : megadirty hack, need to make a line wrapping label
	w->base.e.parent->flags |= UI_PANEL_EXPAND;

	UICode *descrCode = UICodeCreate(w->base.e.parent, UI_CODE_NO_MARGIN | UI_ELEMENT_V_FILL);
	UICodeInsertContent(descrCode, displayedDescr, -1, false);
	descrCode->e.cp = w;
	descrCode->e.messageUser = PWatchViewerCodeMessage;

	UIElementFocus(&descrCode->e);
	UIElementRefresh(descrCode->e.parent);
	UIElementRefresh(&descrCode->e);
}

int PWatchViewerWindowMessage(UIElement *element, UIMessage message, int di, void *dp) {
	PWatchViewerWindow *w = (PWatchViewerWindow *)element->cp;
	int rowHeight = (int)(UI_SIZE_TEXTBOX_HEIGHT * element->window->scale);
	int result = 0;
	if (message == UI_MSG_PAINT) {
		UIPainter *painter = (UIPainter *)dp;

		for (int i = (painter->clip.t - element->bounds.t) / rowHeight; i < w->rows.Length(); i++) {
			UIRectangle row = element->bounds;
			row.t += i * rowHeight, row.b = row.t + rowHeight;

			UIRectangle intersection = UIRectangleIntersection(row, painter->clip);
			if (!UI_RECT_VALID(intersection))
				break;

			bool focused = i == w->base.selectedRow && element->window->focused == element;

			if (focused)
				UIDrawBlock(painter, row, ui.theme.selected);
			UIDrawBorder(painter, row, ui.theme.border, UI_RECT_4(0, 1, 0, 1));

			row.l += UI_SIZE_TEXTBOX_MARGIN;
			row.r -= UI_SIZE_TEXTBOX_MARGIN;

			PWatch *disp = w->rows[i];
			char buffer[256];

			if (disp->updateIndex != w->updateIndex && disp->kind > PWATCH_CLUSTER) {
				if (!programRunning) {
					free(disp->value);
					disp->updateIndex = w->updateIndex;
					StringFormat(buffer, sizeof(buffer), "py gf_read_word(0x%lX,4)", disp->addr);
					EvaluateCommand(buffer);
					char *result = evaluateResult;
					char *end = strchr(result, '\n');
					if (end)
						*end = 0;

					if (0 != strcmp(result, "??")) {

						uint64_t val = atol(result);
						if (disp->kind == PWATCH_BITS) {
							val = (val >> disp->shift) & disp->mask;
						}
						StringFormat(buffer, sizeof(buffer), disp->format, val);

						disp->value = strdup(buffer);
					} else {
						disp->value = strdup("??");
					}

				} else {
					free(disp->value);
					disp->value = strdup("..");
				}
			}

			StringFormat(buffer, sizeof(buffer), "%.*s%s%s %s%s%s", disp->depth * 3,
						 "                                           ",
						 disp->open					 ? "v "
						 : disp->kind != PWATCH_BITS ? "> "
													 : "",
						 disp->key, disp->addedInfo == 0 ? "" : disp->addedInfo,
						 disp->kind <= PWATCH_CLUSTER ? " @" : " = ", disp->value);

			if (focused) {
				UIDrawString(painter, row, buffer, -1, ui.theme.textSelected, UI_ALIGN_LEFT, nullptr);
			} else {
				UIDrawStringHighlighted(painter, row, buffer, -1, 1, NULL);
			}
		}
	} else if (message == UI_MSG_GET_HEIGHT) {
		return w->rows.Length() * rowHeight;
	} else if (message == UI_MSG_LEFT_DOWN) {
		w->base.selectedRow = (element->window->cursorY - element->bounds.t) / rowHeight;

		if (w->base.selectedRow >= 0 && w->base.selectedRow < w->rows.Length()) {
			PWatch *watch = w->rows[w->base.selectedRow];
			int x = (element->window->cursorX - element->bounds.l) / ui.activeFont->glyphWidth;

			if (x >= watch->depth * 3 - 1 && x <= watch->depth * 3 + 1 && watch->kind != PWATCH_BITS) {
				UIKeyTyped m = {0};
				m.code = watch->open ? UI_KEYCODE_LEFT : UI_KEYCODE_RIGHT;
				PWatchViewerWindowMessage(element, UI_MSG_KEY_TYPED, 0, &m);
			}
		}

		UIElementFocus(element);
		UIElementRepaint(element, nullptr);

	} else if (message == UI_MSG_KEY_TYPED) {
		UIKeyTyped *m = (UIKeyTyped *)dp;
		result = 1;
		if (m->code == UI_KEYCODE_RIGHT && w->base.selectedRow != w->rows.Length() &&
			w->rows[w->base.selectedRow]->kind != PWATCH_BITS && !w->rows[w->base.selectedRow]->open) {
			PWatch *disp = w->rows[w->base.selectedRow];
			disp->open = true;
			PWatchViewerInsertChildrenRows(w, disp, w->base.selectedRow + 1, true);
		} else if (m->code == UI_KEYCODE_LEFT && w->base.selectedRow != w->rows.Length() &&
				   w->rows[w->base.selectedRow]->kind != PWATCH_BITS && w->rows[w->base.selectedRow]->open) {
			int end = w->base.selectedRow + 1;

			for (; end < w->rows.Length(); end++) {
				if (w->rows[w->base.selectedRow]->depth >= w->rows[end]->depth) {
					break;
				}
			}

			w->rows.Delete(w->base.selectedRow + 1, end - w->base.selectedRow - 1);
			w->rows[w->base.selectedRow]->open = false;
		} else if (m->code == UI_KEYCODE_ENTER) {
			if (w->base.selectedRow >= 0 && w->base.selectedRow < w->rows.Length()) {
				PWatch *disp = w->rows[w->base.selectedRow];
				UIKeyTyped m = {0};
				m.code = disp->open ? UI_KEYCODE_LEFT : UI_KEYCODE_RIGHT;
				PWatchViewerWindowMessage(element, UI_MSG_KEY_TYPED, 0, &m);
			}
		} else if (m->code == UI_KEYCODE_PAGE_UP) {
			int differentKind = w->base.selectedRow;
			PWatchKind currKind = w->rows[w->base.selectedRow]->kind;
			for (; differentKind >= 0; differentKind--) {
				if (currKind != w->rows[differentKind]->kind) {
					break;
				}
			}
			w->base.selectedRow = differentKind;
		} else if (m->code == UI_KEYCODE_PAGE_DOWN) {
			int differentKind = w->base.selectedRow;
			PWatchKind currKind = w->rows[w->base.selectedRow]->kind;
			for (; differentKind < w->rows.Length(); differentKind++) {
				if (currKind != w->rows[differentKind]->kind) {
					break;
				}
			}
			w->base.selectedRow = differentKind;
		} else if (m->code == UI_KEYCODE_LETTER('P') && !element->window->shift && !element->window->alt &&
				   element->window->ctrl) {
			PWatch *parent = w->rows[w->base.selectedRow]->parent;
			if (parent != nullptr) {
				uintptr_t parentRow = 0;
				w->rows.Contains(parent, &parentRow);
				w->base.selectedRow = parentRow;
			}
		} else if (m->code == UI_KEYCODE_LETTER('C') && !element->window->shift && !element->window->alt &&
				   element->window->ctrl) {
			char *value = strdup(w->rows[w->base.selectedRow]->value);
			_UIClipboardWriteText(w->base.e.window, value);
		} else if (m->code == UI_KEYCODE_LETTER('R') && !element->window->shift && !element->window->alt &&
				   element->window->ctrl) {
			PWatchViewerWindowClear(w);
			WindowSwitch(&fileWindow->base, fileWindow->rows.Length());
			return 1;
		} else if (m->code == UI_KEYCODE_LETTER('A') && !element->window->shift && !element->window->alt &&
				   element->window->ctrl) {
			WindowSwitch(&fileWindow->base, fileWindow->rows.Length());
			return 1;
		} else if (m->code == UI_KEYCODE_LETTER('D') && element->window->shift && !element->window->alt &&
				   element->window->ctrl) {
			PWatchViewerSpawnDescription(w);
		} else {
			result = PWatchKeyboardNavigation(&w->base, m->code, w->rows.Length());
		}

		PWatchEnsureRowVisible(&w->base, w->base.selectedRow, w->rows.Length());
		UIElementRefresh(element->parent);
		UIElementRefresh(element);
	}
	return result;
}

int PWatchFileWindowMessage(UIElement *element, UIMessage message, int di, void *dp) {

	PWatchFileWindow *w = (PWatchFileWindow *)element->cp;
	int rowHeight = (int)(UI_SIZE_TEXTBOX_HEIGHT * element->window->scale);
	int result = 0;
	if (message == UI_MSG_PAINT) {
		UIPainter *painter = (UIPainter *)dp;

		for (int i = (painter->clip.t - element->bounds.t) / rowHeight; i <= w->rows.Length(); i++) {
			UIRectangle row = element->bounds;
			row.t += i * rowHeight, row.b = row.t + rowHeight;

			UIRectangle intersection = UIRectangleIntersection(row, painter->clip);
			if (!UI_RECT_VALID(intersection))
				break;

			bool focused = i == w->base.selectedRow && element->window->focused == element;

			if (focused)
				UIDrawBlock(painter, row, ui.theme.selected);
			UIDrawBorder(painter, row, ui.theme.border, UI_RECT_4(0, 1, 0, 1));
			if (i < w->rows.Length()) {
				row.l += UI_SIZE_TEXTBOX_MARGIN;
				row.r -= UI_SIZE_TEXTBOX_MARGIN;

				PWatchSVDEntry *ent = &w->rows[i];
				char buffer[256];

				StringFormat(buffer, sizeof(buffer), "%s %s", ent->name, ent->source);

				if (focused) {
					UIDrawString(painter, row, buffer, -1, ui.theme.textSelected, UI_ALIGN_LEFT, nullptr);
				} else {
					UIDrawStringHighlighted(painter, row, buffer, -1, 1, NULL);
				}
			}
		}
	} else if (message == UI_MSG_GET_HEIGHT) {
		return (w->rows.Length() + 1) * rowHeight;
	} else if (message == UI_MSG_LEFT_DOWN) {
		int newSelected = (element->window->cursorY - element->bounds.t) / rowHeight;
		if (element->window->focused == element && w->base.selectedRow == newSelected) {
			UIKeyTyped m = {0};
			m.code = UI_KEYCODE_ENTER;
			PWatchFileWindowMessage(element, UI_MSG_KEY_TYPED, 0, &m);
			return 1;
		} else {
			w->base.selectedRow = newSelected;
			UIElementFocus(element);
			UIElementRepaint(element, nullptr);
		}
	} else if (message == UI_MSG_KEY_TYPED) {
		UIKeyTyped *m = (UIKeyTyped *)dp;
		result = 1;
		if (m->code == UI_KEYCODE_ENTER) {
			char concatBuff[PATH_MAX];
			StringFormat(concatBuff, PATH_MAX, "%s/%s", w->rows[w->base.selectedRow].source,
						 w->rows[w->base.selectedRow].name);

			if (PWatchViewerWindowPopulate(viewerWindow, concatBuff)) {
				WindowSwitch(&viewerWindow->base, viewerWindow->rows.Length());
			}

			return 1;
		} else if (m->code == UI_KEYCODE_LETTER('C') && !element->window->shift && !element->window->alt &&
				   element->window->ctrl) {
			char *value = strdup(w->rows[w->base.selectedRow].name);
			_UIClipboardWriteText(w->base.e.window, value);
		} else {
			result = PWatchKeyboardNavigation(&w->base, m->code, w->rows.Length());
			if (w->base.selectedRow == w->rows.Length()) {
				UIElementFocus(&w->textbox->e);
			}
		}

		PWatchEnsureRowVisible(&w->base, w->base.selectedRow, w->rows.Length());
		UIElementRefresh(element->parent);
		UIElementRefresh(element);
	}
	return result;
}

PWatchFileWindow *PWatchFileWindowCreate(UIElement *parent) {
	PWatchFileWindow *w =
		(PWatchFileWindow *)UIElementCreate(sizeof(PWatchFileWindow), parent, UI_ELEMENT_H_FILL | UI_ELEMENT_TAB_STOP,
											PWatchFileWindowMessage, "PWatchFile");
	w->base.e.cp = w;
	return w;
}

PWatchViewerWindow *PWatchViewerWindowCreate(UIElement *parent) {
	PWatchViewerWindow *w = (PWatchViewerWindow *)UIElementCreate(sizeof(PWatchViewerWindow), parent,
																  UI_ELEMENT_H_FILL | UI_ELEMENT_TAB_STOP,
																  PWatchViewerWindowMessage, "PWatch");
	w->base.e.cp = w;
	return w;
}

/////////////////////////////////////////////////////
// Config:
/////////////////////////////////////////////////////

void LoadSVDConfig(SVDConfig *config, const char *iniPath) {
	INIState s = {.buffer = LoadFile(iniPath, &s.bytes)};

	if (!s.buffer)
		return;

	config->configBuffers.Add(s.buffer);

	while (INIParse(&s)) {
		if (0 == strcmp(s.section, "svd")) {
			Array<char *> *target = 0;
			if (0 == strcmp(s.key, "paths")) {
				target = &config->paths;
			} else if (0 == strcmp(s.key, "projectfiles")) {
				target = &config->projectFiles;
			}

			if (target) {
				target->length = 0;
				char *curr = s.value;
				while (char *delim = strchr(curr, ':')) {
					*delim = '\0';
					target->Add(curr);
					curr = delim + 1;
				}
				target->Add(curr);
			}
		}
	}
}

UIElement *PWatchWindowCreate(UIElement *parent) {

	SVDConfig config = {0};

	pluginPanel = UIPanelCreate(parent, UI_PANEL_SCROLL | UI_PANEL_COLOR_1);
	pluginPanel->e.messageUser = PWatchPanelMessage;

	viewerWindow = PWatchViewerWindowCreate(&pluginPanel->e);
	fileWindow = PWatchFileWindowCreate(&pluginPanel->e);

	for (int i = 0; i < 2; i++) {
		LoadSVDConfig(&config, i == 0 ? globalConfigPath : localConfigPath);
	}

	for (int i = 0; i < config.paths.Length(); i++) {
		PWatchFileWindowPopulate(fileWindow, config.paths[i]);
	}

	PWatchFileWindowPopulate(fileWindow, ".");

	int loaded = 0;
	for (int i = 0; i < config.projectFiles.Length(); i++) {
		if (PWatchViewerWindowPopulate(viewerWindow, config.projectFiles[i])) {
			loaded++;
		}
	}

	if (loaded) {
		fileWindow->base.e.flags |= UI_ELEMENT_HIDE;
		pluginPanel->e.cp = viewerWindow;
	} else {
		viewerWindow->base.e.flags |= UI_ELEMENT_HIDE;
		pluginPanel->e.cp = fileWindow;
	}

	if (config.paths.allocated)
		config.paths.Free();
	if (config.projectFiles.allocated)
		config.projectFiles.Free();
	if (config.configBuffers.allocated) {
		for (int i = 0; i < config.configBuffers.Length(); i++) {
			free(config.configBuffers[i]);
		}
		config.configBuffers.Free();
	}
	return &pluginPanel->e;
}

void PWatchWindowFocus(UIElement *element) {
	UIElement *e = (UIElement *)element->cp;
	UIElementFocus(e);
}

void PWatchWindowUpdate(const char *unused, UIElement *element) {
	(void)unused;
	PWatchWindowBase *w = (PWatchWindowBase *)element->cp;

	static bool pwatchPythonInjected = false;
	if (!pwatchPythonInjected) {
		EvaluateCommand(pwatchPythonCode);
		pwatchPythonInjected = true;
	}

	if (!fileWindow->textbox) {
		PWatchFileWindowSpawnTextbox(fileWindow);
	}

	if (w == &viewerWindow->base) {
		((PWatchViewerWindow *)element->cp)->updateIndex++;
	}

	UIElementRefresh(element->parent);
	UIElementRefresh(element);
}

__attribute__((constructor)) void PWatchRegister() {
	interfaceWindows.Add({"PWatch", PWatchWindowCreate, PWatchWindowUpdate, PWatchWindowFocus});
}
