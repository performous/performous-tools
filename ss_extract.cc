#include <stdlib.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <boost/filesystem.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>

#include "chc_decode.hh"
#include "ss_cover.hh"

#include "ss_helpers.hh"

namespace fs = boost::filesystem;

struct Song {
	std::string dataPakName, title, artist, genre, edition, year;
	fs::path path, music, vocals, video, background, cover;
	unsigned samplerate;
	double tempo;
	bool isDuet, pal;
        double medleyStart, medleyEnd;
	Song(): samplerate(), tempo(), isDuet(), medleyStart(), medleyEnd() {}
};

#include "ss_binary.hh"

enum Singer {
	SINGER1,
	SINGER2,
	NUM_SINGERS
};

std::string dvdPath;
std::ofstream txtfile;
std::string singerName[NUM_SINGERS];
std::stringstream singerNotes[NUM_SINGERS];
bool singerActive[NUM_SINGERS];
int ts = 0;
int sleepts = -1;
int pitch_offset = 0;
bool g_video = true;
bool g_audio = true;
bool g_mkvcompress = true;
bool g_mp4compress = true;
bool g_oggcompress = true;
bool g_mp3compress = true;
bool g_createtxt = true;
bool g_duet = true;

void parseNote(xmlpp::Node* node) {
	xmlpp::Element& elem = dynamic_cast<xmlpp::Element&>(*node);
	std::stringstream notes;
	char type = ':';
	std::string lyric = elem.get_attribute("Lyric")->get_value();
	// Some extra formatting to make lyrics look better (hyphen removal & whitespace)
	if (lyric.size() > 0 && lyric[lyric.size() - 1] == '-') {
		if (lyric.size() > 1 && lyric[lyric.size() - 2] == ' ') lyric.erase(lyric.size() - 2);
		else lyric[lyric.size() - 1] = '~';
	} else {
		lyric += ' ';
	}
	unsigned note = boost::lexical_cast<unsigned>(elem.get_attribute("MidiNote")->get_value().c_str());
	unsigned duration = boost::lexical_cast<unsigned>(elem.get_attribute("Duration")->get_value().c_str());
        bool rap = elem.get_attribute("Rap");
        bool golden = elem.get_attribute("Bonus");
        bool freestyle = elem.get_attribute("FreeStyle");
        if (!rap && golden) type = '*';
        else if (rap && !golden) type = 'R';
        else if (rap && golden) type = 'G';
        else if (freestyle) type = 'F';
	if (note) {
		if (sleepts > 0) notes << "- " << sleepts << '\n';
		sleepts = 0;
		notes << type << ' ' << ts << ' ' << duration << ' ' << note - pitch_offset << ' ' << lyric << '\n';
	}
	ts += duration;

	bool written = false;
	for (int i = 0; i < NUM_SINGERS; ++i) {
		if (singerActive[i]) {
			singerNotes[i] << notes.str();
			written = true;
		}
	}
	if (!written)
		throw std::runtime_error("No singer for note");
}

void parseSentence(xmlpp::Node* node, bool withSinger) {
	xmlpp::Element& elem = dynamic_cast<xmlpp::Element&>(*node);
	if (withSinger) {
		xmlpp::Attribute* singerAttr = elem.get_attribute("Singer");
		if (singerAttr) {
			std::string singerStr = singerAttr->get_value();
			for (int i = 0; i < NUM_SINGERS; ++i)
				singerActive[i] = false;
			if (singerStr == "Solo 1") {
				singerActive[SINGER1] = true;
			} else if (singerStr == "Solo 2") {
				singerActive[SINGER2] = true;
			} else if (singerStr == "Group") {
				singerActive[SINGER1] = true;
				singerActive[SINGER2] = true;
			} else throw std::runtime_error("Invalid Singer");
		}
	}
	// FIXME: Get rid of this or use SSDom's find
	xmlpp::Node::PrefixNsMap nsmap;
	nsmap["ss"] = "http://www.singstargame.com";
	auto n = elem.find("ss:NOTE", nsmap);
	if (n.empty()) n = elem.find("NOTE");
	if (sleepts != -1) sleepts = ts;
	std::for_each(n.begin(), n.end(), parseNote);
}

void parseSentenceWithSinger(xmlpp::Node* node) {
	parseSentence(node, true);
}

void parseSentenceWithoutSinger(xmlpp::Node* node) {
	parseSentence(node, false);
}

struct Match {
	std::string left, right;
	Match(std::string l, std::string r): left(l), right(r) {}
	bool operator()(Pak::files_t::value_type const& f) {
		std::string n = f.first;
		return n.substr(0, left.size()) == left && n.substr(n.size() - right.size()) == right;
	}
};

void initTxtFile(const fs::path &path, const Song &song, const std::string suffix = "") {
	fs::path file_path;
	file_path = path / (std::string("notes") + suffix + ".txt");
	txtfile.open(file_path.string().c_str());
	txtfile << "#TITLE:" << song.title << suffix << std::endl;
	txtfile << "#ARTIST:" << song.artist << std::endl;
	if (!song.genre.empty()) txtfile << "#GENRE:" << song.genre << std::endl;
	if (!song.year.empty()) txtfile << "#YEAR:" << song.year << std::endl;
	if (!song.edition.empty()) txtfile << "#EDITION:" << song.edition << std::endl;
	//txtfile << "#LANGUAGE:English" << std::endl; // Detect instead of hardcoding?
	if (!song.music.empty()) txtfile << "#MP3:" << filename(song.music) << std::endl;
	if (!song.vocals.empty()) txtfile << "#VOCALS:" << filename(song.vocals) << std::endl;
	if (!song.video.empty()) txtfile << "#VIDEO:" << filename(song.video) << std::endl;
	if (!song.cover.empty()) txtfile << "#COVER:" << filename(song.cover) << std::endl;
	//txtfile << "#BACKGROUND:background.jpg" << std::endl;
	txtfile << "#BPM:" << song.tempo << std::endl;
	if (song.medleyEnd > 0) {
		int start = std::round(4 * (song.tempo / 60) * song.medleyStart);
		txtfile << "#MEDLEYSTARTBEAT:" << start << std::endl;
		int end = std::round(4 * (song.tempo / 60) * song.medleyEnd);
		txtfile << "#MEDLEYENDBEAT:" << end << std::endl;
	}
}

void finalizeTxtFile() {
	txtfile << 'E' << std::endl;
	txtfile.close();
}

ChcDecode chc_decoder;

struct Process {
	Pak const& pak;
	Process(Pak const& p): pak(p) {}
	void operator()(std::pair<std::string const, Song>& songpair) {
		fs::path remove;
		try {
			std::string const& id = songpair.first;
			Song& song = songpair.second;
			std::cerr << "\n[" << id << "] " << song.artist << " - " << song.title << std::endl;
			fs::path path = safename(song.edition);
			path /= safename(song.artist + " - " + song.title);
			SSDom dom;
			{
				std::vector<char> tmp;
				Pak::files_t::const_iterator it = std::find_if(pak.files().begin(), pak.files().end(), Match("export/" + id + "/melody", ".xml"));
				if (it == pak.files().end()) {
					it = std::find_if(pak.files().begin(), pak.files().end(), Match("export/melodies_10", ".chc"));
					if (it == pak.files().end()) throw std::runtime_error("Melody XML not found");
					it->second.get(tmp);
					dom.load(chc_decoder.getMelody(&tmp[0], tmp.size(), boost::lexical_cast<unsigned int>(id)));
				} else {
					it->second.get(tmp);
					dom.load(xmlFix(tmp));
				}
			}
			if (song.tempo == 0.0) {
				xmlpp::const_NodeSet n;
				dom.find("/ss:MELODY", n) || dom.find("/MELODY", n);
				if (n.empty()) throw std::runtime_error("Unable to find BPM info");
				xmlpp::Element& e = dynamic_cast<xmlpp::Element&>(*n[0]);
				std::string res = e.get_attribute("Resolution")->get_value();
				song.tempo = boost::lexical_cast<double>(e.get_attribute("Tempo")->get_value().c_str());
				if (res == "Semiquaver") {}
				else if (res == "Demisemiquaver") song.tempo *= 2.0;
				else throw std::runtime_error("Unknown tempo resolution: " + res);
				song.isDuet = e.get_attribute("Duet") && e.get_attribute("Duet")->get_value() == "Yes";
			}
			fs::create_directories(path);
			remove = path;
			dom.get_document()->write_to_file((path / "notes.xml").string(), "UTF-8");
			Pak dataPak(song.dataPakName);
			if (g_audio) {
				std::cerr << ">>> Extracting and decoding music" << std::endl;
				try {
					music(song, dataPak[id + "/music.mib"], pak["export/" + id + "/music.mih"], path);
				} catch (...) {
					music_us(song, dataPak[id + "/mus+vid.iav"], dataPak[id + "/mus+vid.ind"], path);
				}
			}
			std::cerr << ">>> Extracting cover image" << std::endl;
			try {
				SingstarCover c = SingstarCover(dvdPath + "/pack_ee.pak", boost::lexical_cast<unsigned int>(id));
				c.write(path / "/cover.png");
				song.cover = path / "cover.png";
			} catch (...) {}
			remove = "";
			// FIXME: use some library (preferrably ffmpeg):
			if (g_oggcompress) {
				if( !song.music.empty() ) {
					std::cerr << ">>> Compressing audio into music.ogg" << std::endl;
					std::string cmd = "oggenc \"" + song.music.string() + "\"";
					std::cerr << cmd << std::endl;
					if (std::system(cmd.c_str()) == 0) { // FIXME: std::system return value is not portable
						fs::remove(song.music);
						song.music = path / ("music.ogg");
					}
				}
				if( !song.vocals.empty() ) {
					std::cerr << ">>> Compressing audio into vocals.ogg" << std::endl;
					std::string cmd = "oggenc \"" + song.vocals.string() + "\"";
					std::cerr << cmd << std::endl;
					if (std::system(cmd.c_str()) == 0) { // FIXME: std::system return value is not portable
						fs::remove(song.vocals);
						song.vocals = path / ("vocals.ogg");
					}
				}
			}
			if (g_mp3compress) {
				if( !song.music.empty() ) {
					std::cerr << ">>> Compressing audio into music.mp3" << std::endl;
					std::string cmd = "lame \"" + song.music.string() + "\"";
					std::cerr << cmd << std::endl;
					if (std::system(cmd.c_str()) == 0) { // FIXME: std::system return value is not portable
						fs::remove(song.music);
						song.music = path / ("music.mp3");
					}
				}
				if( !song.vocals.empty() ) {
					std::cerr << ">>> Compressing audio into vocals.mp3" << std::endl;
					std::string cmd = "lame \"" + song.vocals.string() + "\"";
					std::cerr << cmd << std::endl;
					if (std::system(cmd.c_str()) == 0) { // FIXME: std::system return value is not portable
						fs::remove(song.vocals);
						song.vocals = path / ("vocals.mp3");
					}
				}
			}
			if (g_video) {
				std::cerr << ">>> Extracting video" << std::endl;
				try {
					std::vector<char> ipudata;
					dataPak[id + "/movie.ipu"].get(ipudata);
					std::cerr << ">>> Converting video" << std::endl;
					IPUConv(ipudata, (path / "video.mpg").string());
					song.video = path / "video.mpg";
				} catch (...) {
					std::cerr << "  >>> European DVD failed, trying American (WIP)" << std::endl;
					try {
						video_us(song, dataPak[id + "/mus+vid.iav"], dataPak[id + "/mus+vid.ind"], path);
					} catch (std::exception& e) {
						std::cerr << "!!! Unable to extract video: " << e.what() << std::endl;
						song.video = "";
					}
				}
				if (g_mkvcompress) {
					std::cerr << ">>> Compressing video into video.m4v" << std::endl;
					std::string cmd = "ffmpeg -i \"" + (path / "video.mpg").string() + "\" -vcodec libx264 -profile main -crf 25 -threads 0 -metadata album=\"" + song.edition + "\" -metadata author=\"" + song.artist + "\" -metadata comment=\"" + song.genre + "\" -metadata title=\"" + song.title + "\" \"" + (path / "video.m4v\"").string();
					std::cerr << cmd << std::endl;
					if (std::system(cmd.c_str()) == 0) { // FIXME: std::system return value is not portable
						fs::remove(path / "video.mpg");
						song.video = path / "video.m4v";
					}
				}
				if (g_mp4compress) {
					std::cerr << ">>> Compressing video into video.mp4" << std::endl;
					std::string cmd = "ffmpeg -i \"" + (path / "video.mpg").string() + "\" -vcodec libx264 -profile main -crf 25 -threads 0 -metadata album=\"" + song.edition + "\" -metadata author=\"" + song.artist + "\" -metadata comment=\"" + song.genre + "\" -metadata title=\"" + song.title + "\" \"" + (path / "video.mp4\"").string();
					std::cerr << cmd << std::endl;
					if (std::system(cmd.c_str()) == 0) { // FIXME: std::system return value is not portable
						fs::remove(path / "video.mpg");
						song.video = path / "video.mp4";
					}
				}
			}

			if (g_createtxt) {
				std::cerr << ">>> Extracting lyrics to notes.txt" << std::endl;
				xmlpp::const_NodeSet sentences;

				if (song.isDuet) {
					xmlpp::const_NodeSet tracks;
					if (!dom.find("/ss:MELODY/ss:TRACK", tracks)) throw std::runtime_error("Unable to find any tracks in melody XML");

					if (tracks.size() != NUM_SINGERS)
						throw std::runtime_error("Invalid number of tracks");

					xmlpp::Element *trackElem[NUM_SINGERS];

					auto it = tracks.begin();
					for (int i = 0; i < NUM_SINGERS; ++i) {
						trackElem[i] = dynamic_cast<xmlpp::Element*>(*it++);
						xmlpp::Attribute *artistAttr = trackElem[i]->get_attribute("Artist");
						if (!artistAttr) {
							artistAttr = trackElem[i]->get_attribute("Name");
							if (!artistAttr)
								throw std::runtime_error("Track without Artist");
						}
						singerName[i] = artistAttr->get_value();
						singerActive[i] = false;
					}

					if(dom.find("/ss:MELODY/ss:SENTENCE", sentences)) {
						std::cerr << "  >>> Single-track duet" << std::endl;

						ts = 0;
						sleepts = -1;
						std::for_each(sentences.begin(), sentences.end(), parseSentenceWithSinger);
					} else {
						std::cerr << "  >>> Double-track duet" << std::endl;

						for (int i = 0; i < NUM_SINGERS; ++i) {
							if (!dom.find(*trackElem[i], "ss:SENTENCE", sentences))
								throw std::runtime_error("Unable to find any sentectes inside track in melody XML");
							ts = 0;
							sleepts = -1;
							singerActive[i] = true;
							std::for_each(sentences.begin(), sentences.end(), parseSentenceWithoutSinger);
							singerActive[i] = false;
						}
					}
					if (g_duet) {
						initTxtFile(path, song);
						for (int i = 0; i < NUM_SINGERS; ++i) {
							txtfile << "#P" << i+1 << ": " << singerName[i] << "\n";
						}
						for (int i = 0; i < NUM_SINGERS; ++i) {
							txtfile << "P" << i + 1 << "\n";
							txtfile << singerNotes[i].rdbuf();
						}
						finalizeTxtFile();
					} else {
						for (int i = 0; i < NUM_SINGERS; ++i) {
							initTxtFile(path, song, " (" + singerName[i] + ")");
							txtfile << singerNotes[i].rdbuf();
							finalizeTxtFile();
						}
					}
				} else {
					std::cerr << "  >>> Solo track" << std::endl;

					if(!dom.find("/ss:MELODY/ss:SENTENCE", sentences)) throw std::runtime_error("Unable to find any sentences in melody XML");

					ts = 0;
					sleepts = -1;
					for (int i = 0; i < NUM_SINGERS; ++i)
						singerActive[i] = false;
					singerActive[SINGER1] = true;
					std::for_each(sentences.begin(), sentences.end(), parseSentenceWithoutSinger);
					initTxtFile(path, song);
					txtfile << singerNotes[SINGER1].rdbuf();
					finalizeTxtFile();
				}
			}
		} catch (std::exception& e) {
			std::cerr << e.what() << std::endl;
			if (!remove.empty()) {
				std::cerr << "!!! Removing " << remove.string() << std::endl;
				fs::remove_all(remove);
			}
		}
	}
};

void get_node(const xmlpp::Node* node, std::string& genre, std::string& year)
{
	const xmlpp::ContentNode* nodeContent = dynamic_cast<const xmlpp::ContentNode*>(node);
	const xmlpp::TextNode* nodeText = dynamic_cast<const xmlpp::TextNode*>(node);
	const xmlpp::CommentNode* nodeComment = dynamic_cast<const xmlpp::CommentNode*>(node);

	if(nodeText && nodeText->is_white_space()) //Let's ignore the indenting - you don't always want to do this.
		return;

	//Treat the various node types differently:
	if(nodeText || nodeComment || nodeContent)
	{
		// if any of these exist do nothing! :D
	}
	else if(const xmlpp::Element* nodeElement = dynamic_cast<const xmlpp::Element*>(node))
	{
		//A normal Element node:

		//Print attributes:
		auto attributes = nodeElement->get_attributes();
		for(auto iter = attributes.begin(); iter != attributes.end(); ++iter)
		{
			const xmlpp::Attribute* attribute = *iter;
			if (attribute->get_name() == "GENRE") genre = normalize(attribute->get_value());
			else if (attribute->get_name() == "YEAR") year = normalize(attribute->get_value());
		}
	}

	if(!nodeContent)
	{
		//Recurse through child nodes:
		auto list = node->get_children();
		for(auto iter = list.begin(); iter != list.end(); ++iter)
		{
			get_node(*iter, genre, year); //recursive
		}
	}
}

struct FindSongs {
	std::string edition;
	std::string language;
	std::map<std::string, Song> songs;
	FindSongs(std::string const& search = ""): m_search(search) {}
	void operator()(Pak::files_t::value_type const& p) {
		std::string name = p.first;
		if (name.substr(0, 17) == "export/config.xml"){
			SSDom dom(p.second);  // Read config XML
			// Load decryption keys required for some SingStar games (since 2009 or so)
			std::string keys[4];
			dom.getValue("/ss:CONFIG/ss:PRODUCT_NAME", keys[0]);
			dom.getValue("/ss:CONFIG/ss:PRODUCT_CODE", keys[1]);
			dom.getValue("/ss:CONFIG/ss:TERRITORY", keys[2]);
			dom.getValue("/ss:CONFIG/ss:DEFAULT_LANG", keys[3]);
			chc_decoder.load(keys);
			// Get the singstar edition, use PRODUCT_NAME as fallback for SS Original and SS Party
			if (!dom.getValue("/ss:CONFIG/ss:PRODUCT_DESC", edition)) edition = keys[0];
			if (edition.empty()) throw std::runtime_error("No PRODUCT_DESC or PRODUCT_NAME found");
			edition = prettyEdition(edition);
			std::cout << "### " << edition << std::endl;
			// Get language if available
			language = keys[3];
		}

		if (name.substr(0, 12) != "export/songs" || name.substr(name.size() - 4) != ".xml") return;
		SSDom dom(p.second);  // Read song XML


		xmlpp::const_NodeSet n;
		dom.find("/ss:SONG_SET/ss:SONG", n);
		Song s;
		s.dataPakName = dvdPath + "/pak_iop" + name[name.size() - 5] + ".pak";
		s.edition = edition;
		for (auto it = n.begin(), end = n.end(); it != end; ++it) {
			// Extract song info
			xmlpp::Element& elem = dynamic_cast<xmlpp::Element&>(**it);
			s.title = elem.get_attribute("TITLE")->get_value();
			s.artist = elem.get_attribute("PERFORMANCE_NAME")->get_value();
			if (!m_search.empty() && m_search != elem.get_attribute("ID")->get_value() && (s.artist + " - " + s.title).find(m_search) == std::string::npos) continue;
			xmlpp::Node const* node = dynamic_cast<xmlpp::Node const*>(*it);
			get_node(node, s.genre, s.year); // get the values for genre and year
			// Get video FPS
			double fps = 25.0;
			xmlpp::const_NodeSet fr;
			if (dom.find(elem, "ss:VIDEO/@FRAME_RATE", fr))
			  fps = boost::lexical_cast<double>(dynamic_cast<xmlpp::Attribute&>(*fr[0]).get_value().c_str());
			if (fps == 25.0) s.pal = true;
			const xmlpp::Node::NodeList medleys = elem.get_children("MEDLEYS");
			if (medleys.size() > 0) {
				for (auto const &mt : medleys.front()->get_children("TYPE")) {
					xmlpp::Element& elem = dynamic_cast<xmlpp::Element&>(*mt);
					if (elem.get_first_child_text()->get_content() == "Normal") {
						s.medleyStart = std::stod(elem.get_attribute("Start")->get_value());
						s.medleyEnd = std::stod(elem.get_attribute("End")->get_value());
					}
				}
			}
			// Store song info to songs container
			songs[elem.get_attribute("ID")->get_value()] = s;
		}
	}
  private:
	std::string m_search;
};

int main( int argc, char **argv) {
	std::string video, audio, song;
	namespace po = boost::program_options;
	po::options_description opt("Options");
	opt.add_options()
	  ("help,h", "you are viewing it")
	  ("dvd", po::value<std::string>(&dvdPath), "path to Singstar DVD root")
	  ("list,l", "list tracks only")
	  ("song", po::value<std::string>(&song), "only extract the given track (ID or partial name)")
	  ("video", po::value<std::string>(&video)->default_value("mkv"), "specify video format (none, mkv, mp4, mpeg2)")
	  ("audio", po::value<std::string>(&audio)->default_value("ogg"), "specify audio format (none, ogg, mp3, wav)")
	  ("txt,t", "also convert XML to notes.txt (for UltraStar compatibility)")
	  ("duet,d", "create single duet-mode txt file for duets")
	  ("offset,o", "apply correct pitch offset")
	  ;
	// Process the first flagless option as dvd, the second as song
	po::positional_options_description pos;
	pos.add("dvd", 1);
	pos.add("song", 1);
	po::options_description cmdline;
	cmdline.add(opt);
	po::variables_map vm;
	// Load the arguments
	try {
		po::store(po::command_line_parser(argc, argv).options(opt).positional(pos).run(), vm);
		po::notify(vm);
		if (dvdPath.empty()) throw std::runtime_error("No Singstar DVD path specified. Enter a path to a folder with pack_ee.pak in it.");
		// Process video flag
		if (video == "none") {
			g_video = false;
			g_mkvcompress = false;
			g_mp4compress = false;
		} else if (video == "mkv") {
			g_video = true;
			g_mkvcompress = true;
			g_mp4compress = false;
		} else if (video == "mp4") {
			g_video = true;
			g_mkvcompress = false;
			g_mp4compress = true;
		} else if (video == "mpeg2") {
			g_video = true;
			g_mkvcompress = false;
			g_mp4compress = false;
		} else {
			throw std::runtime_error("Invalid video flag. Value must be {none, mkv, mp4, mpeg2}");
		}
		std::cerr << ">>> Using video flag: \"" << video << "\"" << std::endl;
		// Process audio flag
		if (audio == "none") {
			g_audio = false;
			g_oggcompress = false;
			g_mp3compress = false;
		} else if (audio == "ogg") {
			g_audio = true;
			g_oggcompress = true;
			g_mp3compress = false;
		} else if (audio == "mp3") {
			g_audio = true;
			g_oggcompress = false;
			g_mp3compress = true;
		} else if (audio == "wav") {
			g_audio = true;
			g_oggcompress = false;
			g_mp3compress = false;
		} else {
			throw std::runtime_error("Invalid audio flag. Value must be {none, ogg, mp3, wav}");
		}
		std::cerr << ">>> Using audio flag: \"" << audio << "\"" << std::endl;
		g_createtxt = vm.count("txt") > 0 || vm.count("duet") > 0;
		g_duet = vm.count("duet") > 0;
		pitch_offset = (vm.count("offset") > 0 ? 60 : 0);
		std::cerr << ">>> Convert XML to notes.txt: " << (g_createtxt?"yes":"no") << std::endl;
		std::cerr << ">>> Create single duet-mode txt file for duets: " << (g_duet?"yes":"no") << std::endl;
		std::cerr << ">>> Apply correct pitch offset: " << (pitch_offset?"yes":"no") << std::endl;
	} catch (std::exception& e) {
		std::cout << cmdline << std::endl;
		std::cout << "ERROR: " << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	std::string pack_ee = dvdPath + "/pack_ee.pak"; // Note: lower case (ISO-9660)
	if (!fs::exists(pack_ee)) {
		if (fs::exists(dvdPath + "/Pack_EE.PAK")) { // Note: capitalization (UDF)
			std::cerr <<
			  "Singstar DVDs have UDF and ISO-9660 filesystems on them. Your disc is mounted\n"
			  "as UDF and this causes some garbled data files, making ripping it impossible.\n\n"
			  "Please remount the disc as ISO-9660 and try again. E.g. on Linux:\n"
			  "# mount -t iso9660 /dev/cdrom " << dvdPath << std::endl;
		} else std::cerr << "No Singstar DVD found. Enter a path to a folder with pack_ee.pak in it." << std::endl;
		return EXIT_FAILURE;
	}
	Pak p(pack_ee);
	FindSongs f = std::for_each(p.files().begin(), p.files().end(), FindSongs(song));
	std::cerr << f.songs.size() << " songs found" << std::endl;
	if (vm.count("list")) {
		for( std::map<std::string, Song>::const_iterator it = f.songs.begin() ; it != f.songs.end();  ++it) {
			std::cout << "[" << it->first << "] " << it->second.artist << " - " << it->second.title << std::endl;
		}
	}
	else std::for_each(f.songs.begin(), f.songs.end(), Process(p));
}

