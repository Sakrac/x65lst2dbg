
#include <inttypes.h>
#include <malloc.h>
#include <vector>

#define STRUSE_IMPLEMENTATION
#include "struse/struse.h"

#ifndef _MSC_VER
#define fopen_s(f, n, t) ((*f = fopen(n,t)) == nullptr ? 1 : 0)
#endif

#ifdef __linux__
#include <linux/limits.h>
#define _MAX_PATH PATH_MAX
#endif


void* LoadBinary(const char* filename, size_t& size)
{
	FILE* f;
	if (fopen_s(&f, filename, "rb") == 0) {
		fseek(f, 0, SEEK_END);
		size = ftell(f);
		fseek(f, 0, SEEK_SET);

		void* data = malloc(size);
		fread(data, size, 1, f);
		fclose(f);
		return data;
	}
	return nullptr;
}

struct Section {
	strref name;
	strref type;
	size_t section_id;
	size_t address;
	size_t end_address;
};

struct SectionFileRef {
	strref filename;
	size_t start;
	size_t end;
};

class FileRefSections : public std::vector<SectionFileRef> {

};

struct SectionFileList {
	strref name;
	size_t start, end;
	FileRefSections* files;
};

struct ListObjectAlias {
	strref listing;
	strref object;
};

std::vector<Section> aSections;
std::vector<SectionFileList> aSectionFileLists;
std::vector<ListObjectAlias> aListObjectAliases;
std::vector<char*> aLoaded;

bool ProcessSection(strref& parseOrig, size_t start, strovl& out)
{
	strref parse = parseOrig, parsePrev = parse;
	while (strref line = parse.next_line()) {
		if( line.get_first() == 'S' && line.grab_prefix("Section ")) {
			parseOrig = parsePrev;
			return true;
		}
		if (line.get_first() == '$') {
			++line;
			size_t addr = line.ahextoui();
			out.append('$').append_num((uint32_t)(addr + start), 4, 16).append(line + 4).append("\r\n");
		} else {
			out.append(line).append("\r\n");
		}
		parsePrev = parse;
	}
	parseOrig = parsePrev;
	return true;
}

bool ProcessListing(strref file, strref objFile, strref linkpath, strovl& out)
{
	size_t size;
	strown<_MAX_PATH> filename_local(file);
	filename_local.cleanup_path();

	if (void* file = LoadBinary(filename_local.c_str(), size)) {
		aLoaded.push_back((char*)file);
		strref parse((const char*)file, strl_t(size));
		while (strref line = parse.next_line()) {
			if (line.grab_prefix("Section")) {
				line.skip_whitespace();
				strref name = line.split_label();
				line.skip_whitespace();
				if (line.grab_char('(')) {
					size_t index = line.atoi_skip();
					if (line.grab_char(',')) {
						strref type = line.split_label();
						line.skip_whitespace();
						if (line.grab_char(')')) {
							for (size_t s = 0, n = aSectionFileLists.size(); s < n; ++s) {
								SectionFileList& sfl = aSectionFileLists[s];
								if (sfl.name.same_str_case(name)) {
									for (size_t f = 0, nf = sfl.files->size(); f < nf; ++f) {
										SectionFileRef& sfr = (*sfl.files)[f];
										strown<_MAX_PATH> refname_local(linkpath);
										refname_local.append(sfr.filename);
										refname_local.cleanup_path();
										if (refname_local.same_str(objFile)) {
											printf("Yay?\n");
											//									out.append("Section ");
											out.append("Section ").append(name).sprintf_append(" (%d, ", index);
											out.append(type).append(")\n");
											ProcessSection(parse, sfr.start, out);
										}
									}
								}
							}
						}
					}
				}
			}
		}
	}
	return true;
}

bool MergeListingFiles(const char* linkFile, const char* outName)
{
//	std::vector<SectionFileList> aSectionFileLists;

	void* outFile = malloc(16 * 1024 * 1024);
	strovl out((char*)outFile, 16 * 1024 * 1024);
	strref linkPath;
	int dirEnd = strref(linkFile).find_last('/', '\\');
	if (dirEnd > 0) { linkPath = strref(linkFile, dirEnd + 1); }
	std::vector<strref> processedFiles;
	for (size_t s = 0, n = aSectionFileLists.size(); s < n; ++s) {
		SectionFileList& sfl = aSectionFileLists[s];
		size_t nf = sfl.files->size();
		for (size_t f = 0; f < nf; ++f) {
			SectionFileRef& ref = (*sfl.files)[f];
			strown<_MAX_PATH> fileLocal(linkPath);
			fileLocal.append(ref.filename).cleanup_path();
			// compare with all processed files..
			bool found = false;
			for (size_t c = 0; c < processedFiles.size(); ++c) {
				strown<_MAX_PATH> fileComp(linkPath);
				fileComp.append(processedFiles[c]).cleanup_path();
				if (fileComp.same_str(fileLocal)) {
					found = true;
					break;
				}
			}
			if (!found) {
				printf("Path: " STRREF_FMT "\n", STRREF_ARG(fileLocal));
				processedFiles.push_back(ref.filename);

				for (size_t l = 0, nl = aListObjectAliases.size(); l < nl; ++l) {
					if (fileLocal.same_str(aListObjectAliases[l].object, '/', '\\')) {
						ProcessListing(aListObjectAliases[l].listing, fileLocal.get_strref(), linkPath, out);
						break;
					}
				}
			}
		}
	}
	FILE* f;
	if (fopen_s(&f, outName, "wb") == 0 && f) {
		fwrite(outFile, out.get_len(), 1, f);
		fclose(f);
	} else {
		printf("Failed to open \"%s\" for export\n", outName);
	}
	free(outFile);

	return true;
}


bool LoadFileListFromSection(strref& parseOrig, SectionFileList& list)
{
	strref parse = parseOrig, parsePrev = parse;
	while (strref line = parse.next_line()) {
		if (line.grab_prefix(" + ")) {
			if (line.has_prefix(list.name)) {
				line += list.name.get_len();
				line.skip_whitespace();
				if( line.grab_prefix("from")) {
					line.skip_whitespace();
					int doll = line.find('$');
					if (doll > 0) {
						strref file = line.split(doll);
						file.trim_whitespace();
						++line;
						size_t start = line.ahextoui_skip();
						line.skip_whitespace();
						if (line.grab_char('-')) {
							line.skip_whitespace();
							if (line.grab_char('$')) {
								size_t end = line.ahextoui_skip();
								SectionFileRef ref = { file, start, end };
								list.files->push_back(ref);
							}
						} else { break; }
					} else { break; }
				} else { break; }
			} else { break; }
		} else { break; }
		parsePrev = parse;
	}
	parseOrig = parsePrev;
	return true;
}

bool LoadSectionListing(const char* filename)
{
	size_t size;
	if (void* file = LoadBinary(filename, size)) {
		aLoaded.push_back((char*)file);
		strref parse((const char*)file, strl_t(size));
		while (strref line = parse.next_line()) {
			if (line.grab_prefix("Section ")) {
				line.skip_whitespace();
				strref name = line.split_label();
				line.skip_whitespace();
				if (!line.grab_char('(')) {
					// this is a section file list
					if (line.grab_char('$')) {
						size_t start = line.ahextoui_skip();
						line.skip_whitespace();
						if (line.grab_char('-')) {
							line.skip_whitespace();
							if (line.grab_char('$')) {
								size_t end = line.ahextoui_skip();
								FileRefSections *sections = new FileRefSections(); 
								SectionFileList fileList = { name, start, end, sections };
								aSectionFileLists.push_back(fileList);
								LoadFileListFromSection(parse, aSectionFileLists[aSectionFileLists.size() - 1]);
							}
						}
					}
				} else {
					// this is a section summary
					line.skip_whitespace();
					if (strref::is_number(line.get_first())) {
						int index = line.atoi_skip();
						line.skip_whitespace();
						if (line.grab_char(',')) {
							strref type = line.split_label();
							line.skip_whitespace();
							if (line.grab_prefix("):")) {
								line.skip_whitespace();
								if (line.grab_char('$')) {
									size_t start = line.ahextoui_skip();
									if (line.grab_prefix("-$")) {
										size_t end = line.ahextoui_skip();
										Section section = { name, type, (size_t)index, start, end };
										aSections.push_back(section);
										printf("SECTION " STRREF_FMT " (%d, %x-$%x)\n", STRREF_ARG(name), (int)index, (int)start, (int)end);
									}
								}
							}
						}
					}
				}
			}
		}
		return true;
	}
	return false;
}

bool LoadLstObjFiles(const char* filename)
{
	size_t size;
	if (void* file = LoadBinary(filename, size)) {
		aLoaded.push_back((char*)file);
		strref parse((const char*)file, strl_t(size));
		while (strref line = parse.next_line()) {
			strref a = line.split_token('=');
			a.trim_whitespace();
			line.trim_whitespace();
			if (a && line) {
				ListObjectAlias alias = { a, line };
				aListObjectAliases.push_back(alias);
			}
		}
	}
	return false;
}


int main(int argc, char* argv[])
{
	if (argc < 4) {
		printf("x65lst2dbg link.lst src-to-obj-list.txt out.lst\n");
		return 0;
	}

	LoadSectionListing(argv[1]);
	LoadLstObjFiles(argv[2]);

	MergeListingFiles(argv[1], argv[3]);


	return 0;
}
