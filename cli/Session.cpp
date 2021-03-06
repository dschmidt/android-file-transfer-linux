/*
    This file is part of Android File Transfer For Linux.
    Copyright (C) 2015-2016  Vladimir Menshakov

    Android File Transfer For Linux is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    Android File Transfer For Linux is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Android File Transfer For Linux.
    If not, see <http://www.gnu.org/licenses/>.
 */

#include <cli/Session.h>
#include <cli/CommandLine.h>
#include <cli/PosixStreams.h>
#include <cli/ProgressBar.h>
#include <cli/Tokenizer.h>

#include <mtp/make_function.h>
#include <mtp/ptp/ByteArrayObjectStream.h>
#include <mtp/ptp/ObjectPropertyListParser.h>
#include <mtp/log.h>

#include <sstream>

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <set>

namespace
{
	bool BeginsWith(const std::string &str, const std::string &prefix)
	{
		if (prefix.size() > str.size())
			return false;
		const char *a = str.c_str();
		const char *b = prefix.c_str();
		return strncasecmp(a, b, prefix.size()) == 0;
	}
}


namespace cli
{

	Session::Session(const mtp::DevicePtr &device, bool showPrompt):
		_device(device),
		_session(_device->OpenSession(1)),
		_gdi(_session->GetDeviceInfo()),
		_cs(mtp::Session::AllStorages),
		_cd(mtp::Session::Root),
		_running(true),
		_interactive(isatty(STDOUT_FILENO)),
		_showPrompt(showPrompt)
	{
		using namespace mtp;
		using namespace std::placeholders;
		{
			const char *cols = getenv("COLUMNS");
			_terminalWidth = cols? atoi(cols): 80;
		}

		AddCommand("help", "shows this help",
			make_function([this]() -> void { Help(); }));

		AddCommand("ls", "lists current directory",
			make_function([this]() -> void { List(false); }));
		AddCommand("ls", "<path> lists objects in <path>",
			make_function([this](const Path &path) -> void { List(path, false); }));

		AddCommand("lsext", "lists current directory [extended info]",
			make_function([this]() -> void { List(true); }));
		AddCommand("lsext", "<path> lists objects in <path> [extended info]",
			make_function([this](const Path &path) -> void { List(path, true); }));

		AddCommand("put", "<file> uploads file",
			make_function([this](const LocalPath &path) -> void { Put(path); }));

		AddCommand("put", "put <file> <dir> uploads file to directory",
			make_function([this](const LocalPath &path, const Path &dst) -> void { Put(path, dst); }));

		AddCommand("get", "<file> downloads file",
			make_function([this](const Path &path) -> void { Get(path); }));
		AddCommand("get", "<file> <dst> downloads file to <dst>",
			make_function([this](const Path &path, const LocalPath &dst) -> void { Get(dst, path); }));
		AddCommand("cat", "<file> outputs file",
			make_function([this](const Path &path) -> void { Cat(path); }));

		AddCommand("quit", "quits program",
			make_function([this]() -> void { Quit(); }));
		AddCommand("exit", "exits program",
			make_function([this]() -> void { Quit(); }));

		AddCommand("cd", "<path> change directory to <path>",
			make_function([this](const Path &path) -> void { ChangeDirectory(path); }));
		AddCommand("pwd", "resolved current object directory",
			make_function([this]() -> void { CurrentDirectory(); }));
		AddCommand("rm", "<path> removes object (WARNING: RECURSIVE, be careful!)",
			make_function([this](const LocalPath &path) -> void { Delete(path); }));
		AddCommand("mkdir", "<path> makes directory",
			make_function([this](const Path &path) -> void { MakeDirectory(path); }));
		AddCommand("type", "<path> shows type of file (recognized by libmagic/extension)",
			make_function([this](const LocalPath &path) -> void { ShowType(path); }));

		AddCommand("storage-list", "shows available MTP storages",
			make_function([this]() -> void { ListStorages(); }));
		AddCommand("properties", "<path> lists properties for <path>",
			make_function([this](const Path &path) -> void { ListProperties(path); }));
		AddCommand("device-properties", "shows device's MTP properties",
			make_function([this]() -> void { ListDeviceProperties(); }));

		AddCommand("test-property-list", "test GetObjectPropList on given object",
			make_function([this](const Path &path) -> void { TestObjectPropertyList(path); }));
#if 0
		auto test = [](const std::string &input)
		{
			Tokens tokens;
			Tokenizer(input, tokens);
			print(input);
			for(auto t : tokens)
				print("\t", t);
		};
		AddCommand("test-lexer", "tests lexer",
			make_function([&test]() -> void
			{
				test("a\\ b\\ c d");
				test("\"a b c\" d");
				test("\"\\\"\"");
			}));
#endif
	}

	char ** Session::CompletionCallback(const char *text, int start, int end)
	{
		Tokens tokens;
		Tokenizer(CommandLine::Get().GetLineBuffer(), tokens);
		if (tokens.size() < 2) //0 or 1
		{
			std::string command = !tokens.empty()? tokens.back(): std::string();
			char **comp = static_cast<char **>(calloc(sizeof(char *), _commands.size() + 1));
			auto it = _commands.begin();
			size_t i = 0, n = _commands.size();
			for(; n--; ++it)
			{
				if (end != 0 && !BeginsWith(it->first, command))
					continue;

				comp[i++] = strdup(it->first.c_str());
			}
			if (i == 0) //readline silently dereference matches[0]
			{
				free(comp);
				comp = NULL;
			};
			return comp;
		}
		else
		{
			//completion
			const std::string &commandName = tokens.front();
			auto b = _commands.lower_bound(commandName);
			auto e = _commands.upper_bound(commandName);
			if (b == e)
				return NULL;

			size_t idx = tokens.size() - 2;

			decltype(b) i;
			for(i = b; i != e; ++i)
			{
				if (idx < i->second->GetArgumentCount())
					break;
			}
			if (i == e)
				return NULL;

			//mtp::print("COMPLETING ", commandName, " ", idx, ":", commandName, " with ", i->second->GetArgumentCount(), " args");
			ICommandPtr command = i->second;
			std::list<std::string> matches;
			CompletionContext ctx(*this, idx, text, matches);
			command->Complete(ctx);
			if (ctx.Result.empty())
				return NULL;

			char **comp = static_cast<char **>(calloc(sizeof(char *), ctx.Result.size() + 1));
			size_t dst = 0;
			for(auto i : ctx.Result)
				comp[dst++] = strdup(i.c_str());
			return comp;
		}
		return NULL;
	}

	void Session::ProcessCommand(const std::string &input)
	{
		Tokens tokens;
		Tokenizer(input, tokens);
		if (!tokens.empty())
			ProcessCommand(std::move(tokens));
	}

	void Session::ProcessCommand(Tokens && tokens_)
	{
		Tokens tokens(tokens_);
		if (tokens.empty())
			throw std::runtime_error("no token passed to ProcessCommand");

		std::string cmdName = tokens.front();
		tokens.pop_front();
		auto cmd = _commands.find(cmdName);
		if (cmd == _commands.end())
			throw std::runtime_error("invalid command " + cmdName);

		cmd->second->Execute(tokens);
	}

	void Session::InteractiveInput()
	{
		using namespace mtp;
		if (_interactive && _showPrompt)
		{
			print(_gdi.Manufacturer, " ", _gdi.Model, " ", _gdi.DeviceVersion);
			print("extensions: ", _gdi.VendorExtensionDesc);
			//print(_gdi.SerialNumber); //non-secure
			std::stringstream ss;
			ss << "supported op codes: ";
			for(OperationCode code : _gdi.OperationsSupported)
			{
				ss << hex(code, 4) << " ";
			}
			ss << "\n";
			ss << "supported properties: ";
			for(u16 code : _gdi.DevicePropertiesSupported)
			{
				ss << hex(code, 4) << " ";
			}
			ss << "\n";
			debug(ss.str());
		}

		std::string prompt(_showPrompt? _gdi.Manufacturer + " " + _gdi.Model + "> ": std::string()), input;
		cli::CommandLine::Get().SetCallback([this](const char *text, int start, int end) -> char ** { return CompletionCallback(text, start, end); });

		while (_showPrompt? cli::CommandLine::Get().ReadLine(prompt, input): cli::CommandLine::Get().ReadRawLine(input))
		{
			try
			{
				ProcessCommand(input);
				if (!_running) //do not put newline
					return;
			}
			catch (const mtp::InvalidResponseException &ex)
			{
				mtp::error("error: ", ex.what());
				if (ex.Type == mtp::ResponseType::InvalidStorageID)
					error("\033[1mYour device might be locked or in usb-charging mode, please unlock it and put it in MTP or PTP mode\033[0m\n");
			}
			catch(const std::exception &ex)
			{ error("error: ", ex.what()); }
		}
		if (_showPrompt)
			print("");
	}

	mtp::ObjectId Session::ResolveObjectChild(mtp::ObjectId parent, const std::string &entity)
	{
		//fixme: replace with prop list!
		auto objectList = _session->GetObjectHandles(_cs, mtp::ObjectFormat::Any, parent);
		for(auto object : objectList.ObjectHandles)
		{
			std::string name = _session->GetObjectStringProperty(object, mtp::ObjectProperty::ObjectFilename);
			if (name == entity)
			{
				return object;
			}
		}
		throw std::runtime_error("could not find " + entity + " in path");
	}

	mtp::ObjectId Session::Resolve(const Path &path)
	{
		mtp::ObjectId id = BeginsWith(path, "/")? mtp::Session::Root: _cd;
		for(size_t p = 0; p < path.size(); )
		{
			size_t next = path.find('/', p);
			if (next == path.npos)
				next = path.size();

			std::string entity(path.substr(p, next - p));
			if (entity.empty() || entity == ".")
			{ }
			else
			if (entity == "..")
			{
				id = _session->GetObjectParent(id);
				if (id == mtp::Session::Device)
					id = mtp::Session::Root;
			}
			else
				id = ResolveObjectChild(id, entity);
			p = next + 1;
		}
		return id;
	}

	std::string Session::GetFilename(const std::string &path)
	{
		size_t pos = path.rfind('/');
		return (pos == path.npos)? path: path.substr(pos + 1);
	}

	std::string Session::GetDirname(const std::string &path)
	{
		size_t pos = path.rfind('/');
		return (pos == path.npos)? std::string(): path.substr(0, pos);
	}

	std::string Session::FormatTime(const std::string &timespec)
	{
		if (timespec.size() != 15 || timespec[8] != 'T')
			return timespec;

		return
			timespec.substr(0, 4) + "-" +
			timespec.substr(4, 2) + "-" +
			timespec.substr(6, 2) + " " +
			timespec.substr(9, 2) + ":" +
			timespec.substr(11, 2) + ":" +
			timespec.substr(13, 2);
	}

	mtp::ObjectId Session::ResolvePath(const std::string &path, std::string &file)
	{
		size_t pos = path.rfind('/');
		if (pos == path.npos)
		{
			file = path;
			return _cd;
		}
		else
		{
			file = path.substr(pos + 1);
			return Resolve(path.substr(0, pos));
		}
	}

	void Session::CurrentDirectory()
	{
		std::string path;
		mtp::ObjectId id = _cd;
		while(id != mtp::Session::Device && id != mtp::Session::Root)
		{
			std::string filename = _session->GetObjectStringProperty(id, mtp::ObjectProperty::ObjectFilename);
			path = filename + "/" + path;
			id = _session->GetObjectParent(id);
			if (id == mtp::Session::Device)
				break;
		}
		path = "/" + path;
		mtp::print(path);
	}


	void Session::List(mtp::ObjectId parent, bool extended)
	{
		using namespace mtp;
		if (!extended && _session->GetObjectPropertyListSupported())
		{
			ByteArray data = _session->GetObjectPropertyList(parent, ObjectFormat::Any, ObjectProperty::ObjectFilename, 0, 1);
			ObjectPropertyListParser<std::string> parser;
			//HexDump("list", data, true);
			parser.Parse(data, [](ObjectId objectId, ObjectProperty property, const std::string &name)
			{
				print(std::left, width(objectId, 10), " ", name);
			});
		}
		else
		{
			msg::ObjectHandles handles = _session->GetObjectHandles(_cs, mtp::ObjectFormat::Any, parent);

			for(auto objectId : handles.ObjectHandles)
			{
				try
				{
					msg::ObjectInfo info = _session->GetObjectInfo(objectId);
					if (extended)
						print(
							std::left,
							width(objectId, 10), " ",
							std::right,
							hex(info.ObjectFormat, 4), " ",
							width(info.ObjectCompressedSize, 10), " ",
							FormatTime(info.CaptureDate), " ",
							info.Filename, " ",
							info.ImagePixWidth, "x", info.ImagePixHeight, " "
						);
					else
						print(std::left, width(objectId, 10), " ", info.Filename);
				}
				catch(const std::exception &ex)
				{
					mtp::error("error: ", ex.what());
				}
			}
		}
	}

	void Session::CompletePath(const Path &path, CompletionResult &result)
	{
		std::string filePrefix;
		mtp::ObjectId parent = ResolvePath(path, filePrefix);
		std::string dir = GetDirname(path);
		auto objectList = _session->GetObjectHandles(_cs, mtp::ObjectFormat::Any, parent);
		for(auto object : objectList.ObjectHandles)
		{
			std::string name = _session->GetObjectStringProperty(object, mtp::ObjectProperty::ObjectFilename);
			if (BeginsWith(name, filePrefix))
			{
				if (!dir.empty())
					name = dir + '/' + name;

				mtp::ObjectFormat format = (mtp::ObjectFormat)_session->GetObjectIntegerProperty(object, mtp::ObjectProperty::ObjectFormat);
				if (format == mtp::ObjectFormat::Association)
					name += '/';

				if (name.find(' ') != name.npos)
					result.push_back('"' + name +'"');
				else
					result.push_back(name);
			}
		}
	}

	void Session::ListStorages()
	{
		using namespace mtp;
		msg::StorageIDs list = _session->GetStorageIDs();
		for(size_t i = 0; i < list.StorageIDs.size(); ++i)
		{
			msg::StorageInfo si = _session->GetStorageInfo(list.StorageIDs[i]);
			print(
				std::left, width(list.StorageIDs[i], 8),
				" volume: ", si.VolumeLabel,
				", description: ", si.StorageDescription);
		}
	}

	void Session::Help()
	{
		using namespace mtp;
		print("Available commands are:");
		for(auto i : _commands)
		{
			print("\t", std::left, width(i.first, 20), i.second->GetHelpString());
		}
	}

	void Session::Get(const LocalPath &dst, mtp::ObjectId srcId)
	{
		mtp::ObjectFormat format = static_cast<mtp::ObjectFormat>(_session->GetObjectIntegerProperty(srcId, mtp::ObjectProperty::ObjectFormat));
		if (format == mtp::ObjectFormat::Association)
		{
			mkdir(dst.c_str(), 0700);
			auto obj = _session->GetObjectHandles(_cs, mtp::ObjectFormat::Any, srcId);
			for(auto id : obj.ObjectHandles)
			{
				auto info = _session->GetObjectInfo(id);
				LocalPath dstFile = dst + "/" + info.Filename;
				Get(dstFile, id);
			}
		}
		else
		{
			auto stream = std::make_shared<ObjectOutputStream>(dst);
			if (IsInteractive())
			{
				mtp::u64 size = _session->GetObjectIntegerProperty(srcId, mtp::ObjectProperty::ObjectSize);
				stream->SetTotal(size);
				if (IsInteractive())
					try { stream->SetProgressReporter(ProgressBar(dst, _terminalWidth / 3, _terminalWidth)); } catch(const std::exception &ex) { }
			}
			_session->GetObject(srcId, stream);
		}
	}

	void Session::Get(mtp::ObjectId srcId)
	{
		auto info = _session->GetObjectInfo(srcId);
		Get(LocalPath(info.Filename), srcId);
	}

	void Session::Cat(const Path &path)
	{
		mtp::ByteArrayObjectOutputStreamPtr stream(new mtp::ByteArrayObjectOutputStream);
		_session->GetObject(Resolve(path), stream);
		const mtp::ByteArray & data = stream->GetData();
		std::string text(data.begin(), data.end());
		fputs(text.c_str(), stdout);
		if (text.empty() || text[text.size() - 1] == '\n')
			fputc('\n', stdout);
	}

	mtp::ObjectId Session::MakeOrResolveDirectory(mtp::ObjectId parentId, const std::string &dst, const LocalPath &src)
	{
		try
		{ return _session->CreateDirectory(GetFilename(dst), parentId).ObjectId; }
		catch(const std::exception & ex)
		{ }

		return ResolveObjectChild(parentId, src);
	}

	void Session::Put(mtp::ObjectId parentId, const std::string &dst, const LocalPath &src)
	{
		using namespace mtp;
		struct stat st = {};
		if (stat(src.c_str(), &st))
			throw std::runtime_error(std::string("stat failed: ") + strerror(errno));


		//fixme: dst path was not resolved?

		if (S_ISDIR(st.st_mode))
		{
			mtp::ObjectId dirId = MakeOrResolveDirectory(parentId, dst, src);

			DIR *dir = opendir(src.c_str());
			if (!dir)
			{
				perror("opendir");
				return;
			}
			dirent entry = {};
			while(true)
			{
				dirent *result = NULL;
				if (readdir_r(dir, &entry, &result) != 0)
					throw std::runtime_error(std::string("readdir failed: ") + strerror(errno));
				if (!result)
					break;

				if (strcmp(result->d_name, ".") == 0 || strcmp(result->d_name, "..") == 0)
					continue;

				std::string fname = result->d_name;
				Put(dirId, dst + "/" + fname, src + "/" + fname);
			}
			closedir(dir);
		}
		else
		{
			auto stream = std::make_shared<ObjectInputStream>(src);
			stream->SetTotal(stream->GetSize());

			msg::ObjectInfo oi;
			oi.Filename = GetFilename(dst);
			oi.ObjectFormat = ObjectFormatFromFilename(src);
			oi.SetSize(stream->GetSize());

			if (IsInteractive())
				try { stream->SetProgressReporter(ProgressBar(dst, _terminalWidth / 3, _terminalWidth)); } catch(const std::exception &ex) { }

			_session->SendObjectInfo(oi, mtp::Session::AnyStorage, parentId);
			_session->SendObject(stream);
		}
	}

	void Session::MakeDirectory(mtp::ObjectId parentId, const std::string & name)
	{
		using namespace mtp;
		msg::ObjectInfo oi;
		oi.Filename = name;
		oi.ObjectFormat = ObjectFormat::Association;
		_session->SendObjectInfo(oi, mtp::Session::AnyStorage, parentId);
	}

	void Session::ShowType(const LocalPath &src)
	{
		mtp::ObjectFormat format = mtp::ObjectFormatFromFilename(src);
		print("mtp object format = ", hex(format, 4));
	}

	void Session::ListProperties(mtp::ObjectId id)
	{
		auto ops = _session->GetObjectPropertiesSupported(id);
		std::stringstream ss;
		ss << "properties supported: ";
		for(mtp::ObjectProperty prop: ops.ObjectPropertyCodes)
		{
			ss << mtp::hex(prop, 4) << " ";
		}
		ss << "\n";
		mtp::print(ss.str());
	}

	void Session::ListDeviceProperties()
	{
		using namespace mtp;
		for(u16 code : _gdi.DevicePropertiesSupported)
		{
			print("property code: ", hex(code, 4));
			ByteArray data = _session->GetDeviceProperty((mtp::DeviceProperty)code);
			HexDump("value", data, true);
		}
	}

	namespace
	{
		template<typename>
		struct DummyPropertyListParser
		{
			static mtp::ByteArray Parse(mtp::InputStream &stream, mtp::DataTypeCode dataType)
			{
				switch(dataType)
				{
#define CASE(BITS) \
					case mtp::DataTypeCode::Uint##BITS: \
					case mtp::DataTypeCode::Int##BITS: \
						stream.Skip(BITS / 8); break
					CASE(8); CASE(16); CASE(32); CASE(64); CASE(128);
#undef CASE
					case mtp::DataTypeCode::String:
						stream.ReadString(); break;
					default:
						throw std::runtime_error("got invalid data type code");
				}
				return mtp::ByteArray();
			}
		};
	}

	void Session::GetObjectPropertyList(mtp::ObjectId parent, const std::set<mtp::ObjectId> &originalObjectList, const mtp::ObjectProperty property)
	{
		using namespace mtp;
		print("testing property 0x", hex(property, 4), "...");

		std::set<ObjectId> objectList;
		ByteArray data = _session->GetObjectPropertyList(parent, ObjectFormat::Any, property, 0, 1);
		print("got ", data.size(), " bytes of reply");
		HexDump("property list", data);
		ObjectPropertyListParser<ByteArray, DummyPropertyListParser> parser;

		bool ok = true;

		parser.Parse(data, [this, &objectList, property, &ok](mtp::ObjectId objectId, ObjectProperty p, const ByteArray & ) {
			if ((p == property || property == ObjectProperty::All))
				objectList.insert(objectId);
			else
			{
				print("extra property 0x", hex(p, 4), " returned for object ", objectId.Id, ", while querying property list 0x", mtp::hex(property, 4));
				ok = false;
			}
		});

		std::set<ObjectId> extraData;
		std::set_difference(objectList.begin(), objectList.end(), originalObjectList.begin(), originalObjectList.end(), std::inserter(extraData, extraData.end()));

		if (!extraData.empty())
		{
			print("inconsistent GetObjectPropertyList for property 0x", mtp::hex(property, 4));
			for(auto objectId : extraData)
			{
				print("missing 0x", mtp::hex(property, 4), " for object ", objectId);
				ok = false;
			}
		}
		print("getting object property list of type 0x", hex(property, 4), " ", ok? "PASSED": "FAILED");
	}


	void Session::TestObjectPropertyList(const Path &path)
	{
		using namespace mtp;
		ObjectId id = Resolve(path);
		msg::ObjectHandles oh = _session->GetObjectHandles(mtp::Session::AllStorages, ObjectFormat::Any, id);

		std::set<ObjectId> objectList;
		for(auto id : oh.ObjectHandles)
			objectList.insert(id);

		print("GetObjectHandles ", id, " returns ", oh.ObjectHandles.size(), " objects, ", objectList.size(), " unique");
		GetObjectPropertyList(id, objectList, ObjectProperty::ObjectFilename);
		GetObjectPropertyList(id, objectList, ObjectProperty::ObjectFormat);
		GetObjectPropertyList(id, objectList, ObjectProperty::ObjectSize);
		GetObjectPropertyList(id, objectList, ObjectProperty::DateModified);
		GetObjectPropertyList(id, objectList, ObjectProperty::DateAdded);
		GetObjectPropertyList(id, objectList, ObjectProperty::All);
	}

}
