/*
 * Copyright 2019 Stanislav Pidhorskyi. All rights reserved.
 * License: https://raw.githubusercontent.com/podgorskiy/bimpy/master/LICENSE.txt
 */

#include <pybind11/pybind11.h>
#include <pybind11/operators.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include <memory>
#include <mutex>
#include <fsal.h>
#include <StdFile.h>
#include <cstdio>
#include <sstream>
#ifdef __linux
#include <sys/stat.h>
#endif

namespace py = pybind11;

typedef py::array_t<uint8_t, py::array::c_style> ndarray_uint8;
typedef py::array_t<float, py::array::c_style> ndarray_float32;

int main()
{
	return 0;
}


static fsal::File openfile(const char* filename, fsal::StdFile& tmp_std)
{
	auto fp = std::fopen(filename, "rb");
	if (!fp)
	{
		std::stringstream ss;
		ss << "No such file " << filename;
		PyErr_SetString(PyExc_IOError, ss.str().c_str());
		throw py::error_already_set();
	}
	tmp_std.AssignFile(fp);
	return fsal::File(&tmp_std, fsal::File::borrow{});
}


static py::object read_as_bytes(const fsal::File& fp)
{
	size_t retSize = 0;
	PyBytesObject* bytesObject = nullptr;
	size_t size = fp.GetSize();
	bytesObject = (PyBytesObject*) PyObject_Malloc(offsetof(PyBytesObject, ob_sval) + size + 1);
	PyObject_INIT_VAR(bytesObject, &PyBytes_Type, size);
	bytesObject->ob_shash = -1;
	bytesObject->ob_sval[size] = '\0';

	fp.Read((uint8_t*)bytesObject->ob_sval, size, &retSize);

	if (retSize != size)
	{
		PyErr_SetString(PyExc_IOError, "Error reading file ");
		throw py::error_already_set();
	}

	return py::reinterpret_steal<py::object>((PyObject*)bytesObject);
}

void fix_shape(const py::object& _shape, size_t size, std::vector<size_t>& shape_)
{
	shape_.clear();
	if (!_shape.is(py::none()))
	{
		auto shape = py::cast<py::tuple>(_shape);
		bool has_minus = false;
		size_t mul = 1;
		int minus_id = -1;
		for (int i = 0;  i < shape.size(); ++i)
		{
			auto s = py::cast<ptrdiff_t>(shape[i]);
			if (s < 1)
			{
				if (has_minus)
				{
					std::stringstream ss;
					ss << "Invalid shape ";
					PyErr_SetString(PyExc_IOError, ss.str().c_str());
					throw py::error_already_set();
				}
				has_minus = true;
				minus_id = i;
			}
			else
			{
				mul *= s;
			}

			shape_.push_back(s);
		}
		if (has_minus)
		{
			if (size % mul == 0)
			{
				shape_[minus_id] = size / mul;
			}
			else
			{
				std::stringstream ss;
				ss << "Can't reshape";
				PyErr_SetString(PyExc_IOError, ss.str().c_str());
				throw py::error_already_set();
			}
		}
	}
	else
	{
		shape_.push_back(size);
	}
}


static py::object read_as_numpy_ubyte(const fsal::File& fp, const py::object& _shape)
{
	size_t size = fp.GetSize();

	std::vector<size_t> shape;
	fix_shape(_shape, size, shape);

	ndarray_uint8 data(shape);
	void* ptr = data.request().ptr;
	size_t retSize = -1;
	{
		py::gil_scoped_release release;
		fp.Read((uint8_t*)ptr, size, &retSize);
	}
	if (retSize != size)
	{
		PyErr_SetString(PyExc_IOError, "Error reading file ");
		throw py::error_already_set();
	}
	return data;
}


PYBIND11_MODULE(_vfsdl, m)
{
	m.doc() = "vfsdl - VirtualFSDataLoader";

	m.def("open_as_bytes", [](const char* filename)
	{
		py::gil_scoped_release release;
		fsal::StdFile tmp_std;
		auto fp = openfile(filename, tmp_std);
		return read_as_bytes(fp);
	});

	m.def("open_as_numpy_ubyte", [](const char* filename, py::object shape)
	{
		fsal::StdFile tmp_std;
		fsal::File fp;
		{
			py::gil_scoped_release release;
			fp = openfile(filename, tmp_std);
		}
		return read_as_numpy_ubyte(fp, shape);
	},  py::arg("filename"),  py::arg("shape").none(true) = py::none());

	py::enum_<fsal::Mode>(m, "Mode", py::arithmetic())
		.value("read", fsal::Mode::kRead)
		.value("write", fsal::Mode::kWrite)
		.value("append", fsal::Mode::kAppend)
		.value("read_update", fsal::Mode::kReadUpdate)
		.value("write_update", fsal::Mode::kWriteUpdate)
		.value("append_update", fsal::Mode::kAppendUpdate)
		.export_values();

	py::class_<fsal::Location>(m, "Location")
		.def(py::init<const char*>())
		.def(py::init<const char*, fsal::Location::Options, fsal::PathType, fsal::LinkType>());

	py::implicitly_convertible<const char*, fsal::Location>();

	m.def("open_zip_archive", [](const char* filename)
	{
		fsal::FileSystem fs;
		auto archive_file = fs.Open(filename);
		auto zr = new fsal::ZipReader;
		zr->OpenArchive(archive_file);
		return zr;
	});

	py::class_<fsal::ArchiveReaderInterface> Archive(m, "Archive");
		Archive.def("open", [](fsal::ArchiveReaderInterface& self, const std::string& filepath)->py::object{
			fsal::File f;
			{
				py::gil_scoped_release release;
				f = self.OpenFile(filepath);
			}
			if (f)
			{
				return py::cast(f);
			}
			else
			{
				return py::cast<py::none>(Py_None);
			}
		}, "Opens file")
		.def("open_as_bytes", [](fsal::ArchiveReaderInterface& self, const std::string& filepath)->py::object
		{
			PyBytesObject* bytesObject = nullptr;
			size_t size = 0;
			{
				py::gil_scoped_release release;

				auto alloc = [&size, &bytesObject](size_t s)
				{
					size = s;
					bytesObject = (PyBytesObject*) PyObject_Malloc(offsetof(PyBytesObject, ob_sval) + size + 1);
					PyObject_INIT_VAR(bytesObject, &PyBytes_Type, size);
					bytesObject->ob_shash = -1;
					bytesObject->ob_sval[size] = '\0';
					return bytesObject->ob_sval;
				};

				void* result = self.OpenFile(filepath, alloc);
				if (!result)
				{
					PyObject_Free(bytesObject);
					PyErr_SetString(PyExc_IOError, "Error reading file ");
					throw py::error_already_set();
				}

			}

			return py::reinterpret_steal<py::object>((PyObject*)bytesObject);
		})
		.def("open_as_numpy_ubyte", [](fsal::ArchiveReaderInterface& self, const std::string& filepath, py::object _shape)
		{
			size_t size = 0;
			std::vector<size_t> shape;
			ndarray_uint8 data;
			{
				auto alloc = [&size, &data, &_shape, &shape](size_t s)
				{
					py::gil_scoped_acquire acquire;
					size = s;
					fix_shape(_shape, size, shape);
					data = ndarray_uint8(shape);
					void* ptr = data.request().ptr;
					return ptr;
				};
				{
					py::gil_scoped_release release;
					void* result = self.OpenFile(filepath, alloc);
					if (!result)
					{
						PyErr_SetString(PyExc_IOError, "Error reading file ");
						throw py::error_already_set();
					}
				}
			}
			return data;
		})
		.def("exists", [](fsal::ArchiveReaderInterface& self, const std::string& filepath){
			return self.Exists(filepath);
		}, "Exists")
		.def("list_directory", [](fsal::ArchiveReaderInterface& self, const std::string& filepath){
			return self.Exists(filepath);
		}, "ListDirectory");

	py::class_<fsal::ZipReader>(m, "ZipReader", Archive)
		.def(py::init());

	py::class_<fsal::FileSystem>(m, "FileSystem")
		.def(py::init())
		.def("open", [](fsal::FileSystem& fs, const fsal::Location& location, fsal::Mode mode)->py::object{
				fsal::File f = fs.Open(location, mode);
				if (f)
				{
					return py::cast(f);
				}
				else
				{
					return py::cast<py::none>(Py_None);
				}
			}, py::arg("location"), py::arg("mode") = fsal::kRead, "Opens file")
		.def("exists", &fsal::FileSystem::Exists, "Exists")
		.def("rename", &fsal::FileSystem::Rename, "Rename")
		.def("remove", &fsal::FileSystem::Remove, "Remove")
		.def("create_directory", &fsal::FileSystem::CreateDirectory, "CreateDirectory")
		.def("push_search_path", &fsal::FileSystem::PushSearchPath, "PushSearchPath")
		.def("pop_search_path", &fsal::FileSystem::PopSearchPath, "PopSearchPath")
		.def("clear_search_paths", &fsal::FileSystem::ClearSearchPaths, "ClearSearchPaths")
		.def("mount_archive", &fsal::FileSystem::MountArchive, "AddArchive");

	py::class_<fsal::File>(m, "File")
			.def(py::init())
			.def("read", [](fsal::File& self, ptrdiff_t size) -> py::object
			{
				size_t position = self.Tell();
				size_t file_size = self.GetSize();
				const char* ptr = (const char*)self.GetDataPointer();
				if (size < 0)
				{
					size = file_size - position;
				}
				if (ptr)
				{
					self.Seek(position + size);
					return py::bytes(ptr + position, size); // py::bytes copies data, because PyBytes_FromStringAndSize copies. No way around
				}
				else
				{
					PyBytesObject* bytesObject = (PyBytesObject *)PyObject_Malloc(offsetof(PyBytesObject, ob_sval) + size + 1);
					PyObject_INIT_VAR(bytesObject, &PyBytes_Type, size);
					bytesObject->ob_shash = -1;
					bytesObject->ob_sval[size] = '\0';
					self.Read((uint8_t*)bytesObject->ob_sval, size);
					return py::reinterpret_steal<py::object>((PyObject*)bytesObject);
				}
			}, py::arg("size") = -1, py::return_value_policy::take_ownership)
			.def("seek", [](fsal::File& self, ptrdiff_t offset, int origin){
				self.Seek(offset, fsal::File::Origin(origin));
				return self.Tell();
			}, py::arg("offset"), py::arg("origin") = 0)
			.def("tell", &fsal::File::Tell);

/*
 * ['_CHUNK_SIZE', '__class__', '__del__', '__delattr__', '__dict__', '__dir__', '__doc__', '__enter__', '__eq__', '__exit__',
 * '__format__', '__ge__', '__getattribute__', '__getstate__', '__gt__', '__hash__', '__init__', '__init_subclass__', '__iter__',
 * '__le__', '__lt__', '__ne__', '__new__', '__next__', '__reduce__', '__reduce_ex__', '__repr__', '__setattr__', '__sizeof__',
 * '__str__', '__subclasshook__', '_checkClosed', '_checkReadable', '_checkSeekable', '_checkWritable', '_finalizing', 'buffer',
 * 'close', 'closed', 'detach', 'encoding', 'errors', 'fileno', 'flush', 'isatty', 'line_buffering', 'mode', 'name', 'newlines',
 * 'read', 'readable', 'readline', 'readlines', 'seek', 'seekable', 'tell', 'truncate', 'writable', 'write', 'writelines']

 */
	py::class_<fsal::Status>(m, "Status")
			.def(py::init())
			.def("__nonzero__", &fsal::Status::ok);


//
//py::enum_<ImGuiCond_>(m, "Condition", py::arithmetic())
//.value("Always", ImGuiCond_::ImGuiCond_Always)
//.value("Once", ImGuiCond_::ImGuiCond_Once)
//.value("FirstUseEver", ImGuiCond_::ImGuiCond_FirstUseEver)
//.value("Appearing", ImGuiCond_::ImGuiCond_Appearing)
//.export_values();
//
//py::enum_<ImGuiWindowFlags_>(m, "WindowFlags", py::arithmetic())
//.value("NoTitleBar", ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar)
//.value("NoResize", ImGuiWindowFlags_::ImGuiWindowFlags_NoResize)
//.value("NoMove", ImGuiWindowFlags_::ImGuiWindowFlags_NoMove)
//.value("NoScrollbar", ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar)
//.value("NoScrollWithMouse", ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollWithMouse)
//.value("NoCollapse", ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse)
//.value("AlwaysAutoResize", ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysAutoResize)
//// obsolete --> Set style.FrameBorderSize=1.0f / style.WindowBorderSize=1.0f to enable borders around windows and items
//// .value("ShowBorders", ImGuiWindowFlags_::ImGuiWindowFlags_ShowBorders)
//.value("NoSavedSettings", ImGuiWindowFlags_::ImGuiWindowFlags_NoSavedSettings)
//.value("NoInputs", ImGuiWindowFlags_::ImGuiWindowFlags_NoInputs)
//.value("MenuBar", ImGuiWindowFlags_::ImGuiWindowFlags_MenuBar)
//.value("HorizontalScrollbar", ImGuiWindowFlags_::ImGuiWindowFlags_HorizontalScrollbar)
//.value("NoFocusOnAppearing", ImGuiWindowFlags_::ImGuiWindowFlags_NoFocusOnAppearing)
//.value("NoBringToFrontOnFocus", ImGuiWindowFlags_::ImGuiWindowFlags_NoBringToFrontOnFocus)
//.value("AlwaysVerticalScrollbar", ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysVerticalScrollbar)
//.value("AlwaysHorizontalScrollbar", ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysHorizontalScrollbar)
//.value("AlwaysUseWindowPadding", ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysUseWindowPadding)
//.export_values();
//
//py::enum_<ImGuiInputTextFlags_>(m, "InputTextFlags", py::arithmetic())
//.value("CharsDecimal", ImGuiInputTextFlags_::ImGuiInputTextFlags_CharsDecimal)
//.value("CharsHexadecimal", ImGuiInputTextFlags_::ImGuiInputTextFlags_CharsHexadecimal)
//.value("CharsUppercase", ImGuiInputTextFlags_::ImGuiInputTextFlags_CharsUppercase)
//.value("CharsNoBlank", ImGuiInputTextFlags_::ImGuiInputTextFlags_CharsNoBlank)
//.value("AutoSelectAll", ImGuiInputTextFlags_::ImGuiInputTextFlags_AutoSelectAll)
//.value("EnterReturnsTrue", ImGuiInputTextFlags_::ImGuiInputTextFlags_EnterReturnsTrue)
////.value("CallbackCompletion", ImGuiInputTextFlags_::ImGuiInputTextFlags_CallbackCompletion)
////.value("CallbackHistory", ImGuiInputTextFlags_::ImGuiInputTextFlags_CallbackHistory)
////.value("CallbackAlways", ImGuiInputTextFlags_::ImGuiInputTextFlags_CallbackAlways)
////.value("CallbackCharFilter", ImGuiInputTextFlags_::ImGuiInputTextFlags_CallbackCharFilter)
//.value("AllowTabInput", ImGuiInputTextFlags_::ImGuiInputTextFlags_AllowTabInput)
//.value("CtrlEnterForNewLine", ImGuiInputTextFlags_::ImGuiInputTextFlags_CtrlEnterForNewLine)
//.value("NoHorizontalScroll", ImGuiInputTextFlags_::ImGuiInputTextFlags_NoHorizontalScroll)
//.value("AlwaysInsertMode", ImGuiInputTextFlags_::ImGuiInputTextFlags_AlwaysInsertMode)
//.value("ReadOnly", ImGuiInputTextFlags_::ImGuiInputTextFlags_ReadOnly)
//.value("Password", ImGuiInputTextFlags_::ImGuiInputTextFlags_Password)
////.value("NoUndoRedo", ImGuiInputTextFlags_::ImGuiInputTextFlags_NoUndoRedo)
//.value("Multiline", ImGuiInputTextFlags_::ImGuiInputTextFlags_Multiline)
//.export_values();
//
//py::enum_<ImGuiCol_>(m, "Colors")
//.value("Text", ImGuiCol_::ImGuiCol_Text)
//.value("TextDisabled", ImGuiCol_::ImGuiCol_TextDisabled)
//.value("WindowBg", ImGuiCol_::ImGuiCol_WindowBg)
//.value("ChildBg", ImGuiCol_::ImGuiCol_ChildBg)
//.value("PopupBg", ImGuiCol_::ImGuiCol_PopupBg)
//.value("Border", ImGuiCol_::ImGuiCol_Border)
//.value("BorderShadow", ImGuiCol_::ImGuiCol_BorderShadow)
//.value("FrameBg", ImGuiCol_::ImGuiCol_FrameBg)
//.value("FrameBgHovered", ImGuiCol_::ImGuiCol_FrameBgHovered)
//.value("FrameBgActive", ImGuiCol_::ImGuiCol_FrameBgActive)
//.value("TitleBg", ImGuiCol_::ImGuiCol_TitleBg)
//.value("TitleBgActive", ImGuiCol_::ImGuiCol_TitleBgActive)
//.value("TitleBgCollapsed", ImGuiCol_::ImGuiCol_TitleBgCollapsed)
//.value("MenuBarBg", ImGuiCol_::ImGuiCol_MenuBarBg)
//.value("ScrollbarBg", ImGuiCol_::ImGuiCol_ScrollbarBg)
//.value("ScrollbarGrab", ImGuiCol_::ImGuiCol_ScrollbarGrab)
//.value("ScrollbarGrabHovered", ImGuiCol_::ImGuiCol_ScrollbarGrabHovered)
//.value("ScrollbarGrabActive", ImGuiCol_::ImGuiCol_ScrollbarGrabActive)
//.value("CheckMark", ImGuiCol_::ImGuiCol_CheckMark)
//.value("SliderGrab", ImGuiCol_::ImGuiCol_SliderGrab)
//.value("SliderGrabActive", ImGuiCol_::ImGuiCol_SliderGrabActive)
//.value("Button", ImGuiCol_::ImGuiCol_Button)
//.value("ButtonHovered", ImGuiCol_::ImGuiCol_ButtonHovered)
//.value("ButtonActive", ImGuiCol_::ImGuiCol_ButtonActive)
//.value("Header", ImGuiCol_::ImGuiCol_Header)
//.value("HeaderHovered", ImGuiCol_::ImGuiCol_HeaderHovered)
//.value("HeaderActive", ImGuiCol_::ImGuiCol_HeaderActive)
//.value("Separator", ImGuiCol_::ImGuiCol_Separator)
//.value("SeparatorHovered", ImGuiCol_::ImGuiCol_SeparatorHovered)
//.value("SeparatorActive", ImGuiCol_::ImGuiCol_SeparatorActive)
//.value("ResizeGrip", ImGuiCol_::ImGuiCol_ResizeGrip)
//.value("ResizeGripHovered", ImGuiCol_::ImGuiCol_ResizeGripHovered)
//.value("ResizeGripActive", ImGuiCol_::ImGuiCol_ResizeGripActive)
//.value("PlotLines", ImGuiCol_::ImGuiCol_PlotLines)
//.value("PlotLinesHovered", ImGuiCol_::ImGuiCol_PlotLinesHovered)
//.value("PlotHistogram", ImGuiCol_::ImGuiCol_PlotHistogram)
//.value("PlotHistogramHovered", ImGuiCol_::ImGuiCol_PlotHistogramHovered)
//.value("TextSelectedBg", ImGuiCol_::ImGuiCol_TextSelectedBg)
//.value("DragDropTarget", ImGuiCol_::ImGuiCol_DragDropTarget)
//.value("NavHighlight", ImGuiCol_::ImGuiCol_NavHighlight)
//.value("NavWindowingHighlight", ImGuiCol_::ImGuiCol_NavWindowingHighlight)
//.value("NavWindowingDimBg", ImGuiCol_::ImGuiCol_NavWindowingDimBg)
//.value("ModalWindowDimBg", ImGuiCol_::ImGuiCol_ModalWindowDimBg)
//// Obsolete names (will be removed)
//.value("ChildWindowBg", ImGuiCol_::ImGuiCol_ChildWindowBg)
//.value("Column", ImGuiCol_::ImGuiCol_Column)
//.value("ColumnHovered", ImGuiCol_::ImGuiCol_ColumnHovered)
//.value("ColumnActive", ImGuiCol_::ImGuiCol_ColumnActive)
//.value("ModalWindowDarkening", ImGuiCol_::ImGuiCol_ModalWindowDarkening)
//// [unused since 1.60+] the close button now uses regular button colors.
////.value("CloseButton", ImGuiCol_::ImGuiCol_CloseButton)
////.value("CloseButtonHovered", ImGuiCol_::ImGuiCol_CloseButtonHovered)
////.value("CloseButtonActive", ImGuiCol_::ImGuiCol_CloseButtonActive)
//// [unused since 1.53+] ComboBg has been merged with PopupBg, so a redirect isn't accurate.
////.value("ComboBg", ImGuiCol_::ImGuiCol_ComboBg)
//.export_values();
//
//py::enum_<ImGuiStyleVar_>(m, "Style")
//.value("Alpha", ImGuiStyleVar_::ImGuiStyleVar_Alpha)
//.value("WindowPadding", ImGuiStyleVar_::ImGuiStyleVar_WindowPadding)
//.value("WindowRounding", ImGuiStyleVar_::ImGuiStyleVar_WindowRounding)
//.value("WindowBorderSize", ImGuiStyleVar_::ImGuiStyleVar_WindowBorderSize)
//.value("WindowMinSize", ImGuiStyleVar_::ImGuiStyleVar_WindowMinSize)
//.value("WindowTitleAlign", ImGuiStyleVar_::ImGuiStyleVar_WindowTitleAlign)
////.value("ChildWindowRounding", ImGuiStyleVar_::ImGuiStyleVar_ChildWindowRounding)
//.value("ChildRounding", ImGuiStyleVar_::ImGuiStyleVar_ChildRounding)
//.value("ChildBorderSize", ImGuiStyleVar_::ImGuiStyleVar_ChildBorderSize)
//.value("PopupRounding", ImGuiStyleVar_::ImGuiStyleVar_PopupRounding)
//.value("PopupBorderSize", ImGuiStyleVar_::ImGuiStyleVar_PopupBorderSize)
//.value("FramePadding", ImGuiStyleVar_::ImGuiStyleVar_FramePadding)
//.value("FrameRounding", ImGuiStyleVar_::ImGuiStyleVar_FrameRounding)
//.value("FrameBorderSize", ImGuiStyleVar_::ImGuiStyleVar_FrameBorderSize)
//.value("ItemSpacing", ImGuiStyleVar_::ImGuiStyleVar_ItemSpacing)
//.value("ItemInnerSpacing", ImGuiStyleVar_::ImGuiStyleVar_ItemInnerSpacing)
//.value("IndentSpacing", ImGuiStyleVar_::ImGuiStyleVar_IndentSpacing)
//.value("ScrollbarSize", ImGuiStyleVar_::ImGuiStyleVar_ScrollbarSize)
//.value("ScrollbarRounding", ImGuiStyleVar_::ImGuiStyleVar_ScrollbarRounding)
//.value("GrabMinSize", ImGuiStyleVar_::ImGuiStyleVar_GrabMinSize)
//.value("GrabRounding", ImGuiStyleVar_::ImGuiStyleVar_GrabRounding)
//.value("ButtonTextAlign", ImGuiStyleVar_::ImGuiStyleVar_ButtonTextAlign)
//.export_values();
//
//py::enum_<ImGuiFocusedFlags_>(m,"FocusedFlags")
//.value("None", ImGuiFocusedFlags_None)
//.value("ChildWindows", ImGuiFocusedFlags_ChildWindows)
//.value("RootWindow", ImGuiFocusedFlags_RootWindow)
//.value("AnyWindow", ImGuiFocusedFlags_AnyWindow)
//.value("RootAndChildWindows", ImGuiFocusedFlags_RootAndChildWindows)
//.export_values();
//
//py::enum_<ImGuiHoveredFlags_>(m,"HoveredFlags")
//.value("None",ImGuiHoveredFlags_None)
//.value("ChildWindows",ImGuiHoveredFlags_ChildWindows)
//.value("RootWindow",ImGuiHoveredFlags_RootWindow)
//.value("AnyWindow",ImGuiHoveredFlags_AnyWindow)
//.value("AllowWhenBlockedByPopup",ImGuiHoveredFlags_AllowWhenBlockedByPopup)
//.value("AllowWhenBlockedByActiveItem",ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
//.value("AllowWhenOverlapped",ImGuiHoveredFlags_AllowWhenOverlapped)
//.value("AllowWhenDisabled",ImGuiHoveredFlags_AllowWhenDisabled)
//.value("RectOnly",ImGuiHoveredFlags_RectOnly)
//.export_values();
//
//py::class_<Context>(m, "Context")
//.def(py::init())
//.def("init", &Context::Init, "Initializes context and creates window")
//.def("new_frame", &Context::NewFrame, "Starts a new frame. NewFrame must be called before any imgui functions")
//.def("render", &Context::Render, "Finilizes the frame and draws all UI. Render must be called after all imgui functions")
//.def("should_close", &Context::ShouldClose)
//.def("width", &Context::GetWidth)
//.def("height", &Context::GetHeight)
//.def("__enter__", &Context::NewFrame)
//.def("__exit__", [](Context& self, py::object, py::object, py::object)
//{
//self.Render();
//});
//
//py::enum_<ImDrawCornerFlags_>(m, "Corner")
//.value("TopLeft", ImDrawCornerFlags_TopLeft)
//.value("TopRight", ImDrawCornerFlags_TopRight)
//.value("BotRight", ImDrawCornerFlags_BotRight)
//.value("BotLeft", ImDrawCornerFlags_BotLeft)
//.value("All", ImDrawCornerFlags_All)
//.export_values();
//
//py::class_<Bool>(m, "Bool")
//.def(py::init())
//.def(py::init<bool>())
//.def_readwrite("value", &Bool::value);
//
//py::class_<Float>(m, "Float")
//.def(py::init())
//.def(py::init<float>())
//.def_readwrite("value", &Float::value);
//
//py::class_<Int>(m, "Int")
//.def(py::init())
//.def(py::init<int>())
//.def_readwrite("value", &Int::value);
//
//py::class_<String>(m, "String")
//.def(py::init())
//.def(py::init<std::string>())
//.def_readwrite("value", &String::value);
//
//py::class_<ImVec2>(m, "Vec2")
//.def(py::init())
//.def(py::init<float, float>())
//.def_readwrite("x", &ImVec2::x)
//.def_readwrite("y", &ImVec2::y)
//.def(py::self * float())
//.def(py::self / float())
//.def(py::self + py::self)
//.def(py::self - py::self)
//.def(py::self * py::self)
//.def(py::self / py::self)
//.def(py::self += py::self)
//.def(py::self -= py::self)
//.def(py::self *= float())
//.def(py::self /= float())
//.def("__mul__", [](float b, const ImVec2 &a) {
//return a * b;
//}, py::is_operator());
//
//py::class_<ImVec4>(m, "Vec4")
//.def(py::init())
//.def(py::init<float, float, float, float>())
//.def_readwrite("x", &ImVec4::x)
//.def_readwrite("y", &ImVec4::y)
//.def_readwrite("z", &ImVec4::z)
//.def_readwrite("w", &ImVec4::w);
//
//py::class_<Image>(m, "Image")
//.def(py::init<ndarray_uint8>(), "Constructs Image object from ndarray, PIL Image, numpy array, etc.")
//.def("grayscale_to_alpha", &Image::GrayScaleToAlpha, "For grayscale images, uses values as alpha")
//.def_readonly("width", &Image::m_width)
//.def_readonly("height", &Image::m_height);
//
//py::class_<ImGuiStyle>(m, "GuiStyle")
//.def(py::init())
//.def_readwrite("alpha", &ImGuiStyle::Alpha)
//.def_readwrite("window_padding", &ImGuiStyle::WindowPadding)
//.def_readwrite("window_rounding", &ImGuiStyle::WindowRounding)
//.def_readwrite("window_border_size", &ImGuiStyle::WindowBorderSize)
//.def_readwrite("window_min_size", &ImGuiStyle::WindowMinSize)
//.def_readwrite("window_title_align", &ImGuiStyle::WindowTitleAlign)
//.def_readwrite("child_rounding", &ImGuiStyle::ChildRounding)
//.def_readwrite("child_border_size", &ImGuiStyle::ChildBorderSize)
//.def_readwrite("popup_rounding", &ImGuiStyle::PopupRounding)
//.def_readwrite("popup_border_size", &ImGuiStyle::PopupBorderSize)
//.def_readwrite("frame_padding", &ImGuiStyle::FramePadding)
//.def_readwrite("frame_rounding", &ImGuiStyle::FrameRounding)
//.def_readwrite("frame_border_size", &ImGuiStyle::FrameBorderSize)
//.def_readwrite("item_spacing", &ImGuiStyle::ItemSpacing)
//.def_readwrite("item_inner_spacing", &ImGuiStyle::ItemInnerSpacing)
//.def_readwrite("touch_extra_spacing", &ImGuiStyle::TouchExtraPadding)
//.def_readwrite("indent_spacing", &ImGuiStyle::IndentSpacing)
//.def_readwrite("columns_min_spacing", &ImGuiStyle::ColumnsMinSpacing)
//.def_readwrite("scroll_bar_size", &ImGuiStyle::ScrollbarSize)
//.def_readwrite("scroll_bar_rounding", &ImGuiStyle::ScrollbarRounding)
//.def_readwrite("grab_min_size", &ImGuiStyle::GrabMinSize)
//.def_readwrite("grab_rounding", &ImGuiStyle::GrabRounding)
//.def_readwrite("button_text_align", &ImGuiStyle::ButtonTextAlign)
//.def_readwrite("display_window_padding", &ImGuiStyle::DisplayWindowPadding)
//.def_readwrite("display_safe_area_padding", &ImGuiStyle::DisplaySafeAreaPadding)
//.def_readwrite("mouse_cursor_scale", &ImGuiStyle::MouseCursorScale)
//.def_readwrite("anti_aliased_lines", &ImGuiStyle::AntiAliasedLines)
//.def_readwrite("anti_aliased_fill", &ImGuiStyle::AntiAliasedFill)
//.def_readwrite("curve_tessellation_tol", &ImGuiStyle::CurveTessellationTol)
//.def("get_color",[](ImGuiStyle& self, ImGuiCol_ a)
//{
//return self.Colors[(int)a];
//})
//.def("set_color",[](ImGuiStyle& self, ImGuiCol_ a, ImVec4 c)
//{
//self.Colors[(int)a] = c;
//});
//
//m.def("get_style", &ImGui::GetStyle);
//m.def("set_style", [](const ImGuiStyle& a)
//{
//ImGui::GetStyle() = a;
//});
//
//m.def("style_color_classic", []()
//{
//ImGui::StyleColorsClassic();
//});
//m.def("style_color_dark", []()
//{
//ImGui::StyleColorsDark();
//});
//m.def("style_color_light", []()
//{
//ImGui::StyleColorsLight();
//});
//
//m.def("show_test_window", [](){ ImGui::ShowDemoWindow(); }, "create demo/test window (previously called ShowTestWindow). demonstrate most ImGui features.");	// deprecated
//m.def("show_demo_window", [](){ ImGui::ShowDemoWindow(); }, "create demo/test window (previously called ShowTestWindow). demonstrate most ImGui features.");
//m.def("show_metrics_window", [](){ ImGui::ShowMetricsWindow(); }, "create metrics window. display ImGui internals: draw commands (with individual draw calls and vertices), window list, basic internal state, etc.");
//m.def("show_style_editor", [](){ ImGui::ShowStyleEditor(); }, "add style editor block (not a window). you can pass in a reference ImGuiStyle structure to compare to, revert to and save to (else it uses the default style)");
//m.def("show_style_selector", [](const char* label){ ImGui::ShowStyleSelector(label); }, "add style selector block (not a window), essentially a combo listing the default styles.");
//m.def("show_font_selector", [](const char* label){ ImGui::ShowFontSelector(label); }, "add font selector block (not a window), essentially a combo listing the loaded fonts.");
//m.def("show_user_guide", [](){ ImGui::ShowUserGuide(); }, "add basic help/info block (not a window): how to manipulate ImGui as a end-user (mouse/keyboard controls).");
//
//
//m.def("begin",[](const std::string& name, Bool& opened, ImGuiWindowFlags flags) -> bool
//{
//return ImGui::Begin(name.c_str(), opened.null ? nullptr : &opened.value, flags);
//},
//"Push a new ImGui window to add widgets to",
//py::arg("name"), py::arg("opened") = null, py::arg("flags") = ImGuiWindowFlags_(0));
//
//m.def("end", &ImGui::End);
//
//m.def("begin_child",[](const std::string& str_id, const ImVec2& size, bool border, ImGuiWindowFlags extra_flags) -> bool
//{
//return ImGui::BeginChild(str_id.c_str(), size, border);
//},
//"begin a scrolling region. size==0.0f: use remaining window size, size<0.0f: use remaining window size minus abs(size). size>0.0f: fixed size. each axis can use a different mode, e.g. ImVec2(0,400).",
//py::arg("str_id"), py::arg("size") = ImVec2(0,0), py::arg("border") = false, py::arg("extra_flags") = ImGuiWindowFlags_(0));
//
//m.def("end_child", &ImGui::EndChild);
//
//m.def("begin_main_menu_bar", &ImGui::BeginMainMenuBar, "create and append to a full screen menu-bar. only call EndMainMenuBar() if this returns true!");
//m.def("end_main_menu_bar", &ImGui::EndMainMenuBar);
//m.def("begin_menu_bar", &ImGui::BeginMenuBar, "append to menu-bar of current window (requires ImGuiWindowFlags_MenuBar flag set on parent window). only call EndMenuBar() if this returns true!");
//m.def("end_menu_bar", &ImGui::EndMenuBar);
//m.def("begin_menu",[](const std::string& name, Bool& enabled) -> bool
//{
//return ImGui::BeginMenu(name.c_str(), (bool *) (enabled.null ? nullptr : &enabled.value));
//},
//"create a sub-menu entry. only call EndMenu() if this returns true!",
//py::arg("name"), py::arg("enabled") = Bool(true));
//m.def("menu_item",[](const std::string& label, const std::string& shortcut, Bool& selected, bool enabled) -> bool
//{
//return ImGui::MenuItem(label.c_str(), shortcut.c_str(), selected.null ? nullptr : &selected.value, enabled);
//},
//"return true when activated + toggle (*p_selected) if p_selected != NULL",
//py::arg("name"), py::arg("shortcut"), py::arg("selected") = Bool(false), py::arg("enabled") = true);
//m.def("end_menu", &ImGui::EndMenu);
//
//m.def("begin_tooltip", &ImGui::BeginTooltip);
//m.def("end_tooltip", &ImGui::EndTooltip);
//m.def("set_tooltip", [](const char* text){ ImGui::SetTooltip("%s", text); });
//
//
//m.def("open_popup", [](std::string str_id)
//{
//ImGui::OpenPopup(str_id.c_str());
//},
//"call to mark popup as open (don't call every frame!). popups are closed when user click outside, or if CloseCurrentPopup() is called within a BeginPopup()/EndPopup() block. By default, Selectable()/MenuItem() are calling CloseCurrentPopup(). Popup identifiers are relative to the current ID-stack (so OpenPopup and BeginPopup needs to be at the same level)."
//);
//m.def("open_popup_on_item_click", [](std::string str_id = "", int mouse_button = 1)
//{
//ImGui::OpenPopupOnItemClick(str_id.c_str(), mouse_button);
//},
//"helper to open popup when clicked on last item. return true when just opened."
//);
//m.def("begin_popup", [](std::string str_id, ImGuiWindowFlags flags)->bool
//{
//return ImGui::BeginPopup(str_id.c_str(), flags);
//},
//"",
//py::arg("name"), py::arg("flags") = ImGuiWindowFlags_(0)
//);
//m.def("begin_popup_modal", [](std::string name = "")->bool
//{
//return ImGui::BeginPopupModal(name.c_str());
//},
//""
//);
//
//// add more arguments later:
//m.def("begin_popup_context_item", []()
//{
//ImGui::BeginPopupContextItem();
//},
//"helper to open and begin popup when clicked on last item. if you can pass a NULL str_id only if the previous item had an id. If you want to use that on a non-interactive item such as Text() you need to pass in an explicit ID here."
//);
//m.def("begin_popup_context_window", []()
//{
//ImGui::BeginPopupContextWindow();
//},
//"helper to open and begin popup when clicked on current window."
//);
//m.def("begin_popup_context_void", []()
//{
//ImGui::BeginPopupContextVoid();
//},
//"helper to open and begin popup when clicked in void (where there are no imgui windows)."
//);
//
//m.def("end_popup", &ImGui::EndPopup);
//m.def("is_popup_open", [](std::string str_id = "")->bool
//{
//return ImGui::IsPopupOpen(str_id.c_str());
//},
//""
//);
//m.def("close_current_popup", &ImGui::CloseCurrentPopup);
//
//m.def("get_content_region_max", &ImGui::GetContentRegionMax);
//m.def("get_content_region_avail", &ImGui::GetContentRegionAvail);
//m.def("get_content_region_avail_width", &ImGui::GetContentRegionAvailWidth);
//m.def("get_window_content_region_min", &ImGui::GetWindowContentRegionMin);
//m.def("get_window_content_region_max", &ImGui::GetWindowContentRegionMax);
//m.def("get_window_content_region_width", &ImGui::GetWindowContentRegionWidth);
//// m.def("get_window_font_size", &ImGui::GetWindowFontSize);
//m.def("get_font_size", &ImGui::GetFontSize);
//m.def("set_window_font_scale", &ImGui::SetWindowFontScale);
//m.def("get_window_pos", &ImGui::GetWindowPos);
//m.def("get_window_size", &ImGui::GetWindowSize);
//m.def("get_window_width", &ImGui::GetWindowWidth);
//m.def("get_window_height", &ImGui::GetWindowHeight);
//m.def("is_window_collapsed", &ImGui::IsWindowCollapsed);
//m.def("is_window_appearing", &ImGui::IsWindowAppearing);
//m.def("is_window_focused", &ImGui::IsWindowFocused);
//m.def("is_window_hovered", &ImGui::IsWindowHovered);
//
//m.def("set_window_font_scale", &ImGui::SetWindowFontScale);
//
//m.def("set_next_window_pos", &ImGui::SetNextWindowPos, py::arg("pos"), py::arg("cond") = 0, py::arg("pivot") = ImVec2(0,0));
//m.def("set_next_window_size", &ImGui::SetNextWindowSize, py::arg("size"), py::arg("cond") = 0);
//m.def("set_next_window_size_constraints", [](const ImVec2& size_min, const ImVec2& size_max){ ImGui::SetNextWindowSizeConstraints(size_min, size_max); }, py::arg("size_min"), py::arg("size_max") = 0);
//m.def("set_next_window_content_size", &ImGui::SetNextWindowContentSize, py::arg("size"));
//m.def("set_next_window_content_width", &ImGui::SetNextWindowContentWidth, py::arg("width"));
//m.def("set_next_window_collapsed", &ImGui::SetNextWindowCollapsed, py::arg("collapsed"), py::arg("cond") = 0);
//m.def("set_next_window_focus", &ImGui::SetNextWindowFocus);
//m.def("set_window_pos", [](const char* name, const ImVec2& pos, ImGuiCond cond){ ImGui::SetWindowPos(name, pos, cond); }, py::arg("name"), py::arg("pos"), py::arg("cond") = 0);
//m.def("set_window_size", [](const char* name, const ImVec2& size, ImGuiCond cond){ ImGui::SetWindowSize(name, size, cond); }, py::arg("name"), py::arg("size"), py::arg("cond") = 0);
//m.def("set_window_collapsed", [](const char* name, bool collapsed, ImGuiCond cond){ ImGui::SetWindowCollapsed(name, collapsed, cond); }, py::arg("name"), py::arg("collapsed"), py::arg("cond") = 0);
//m.def("set_window_focus", [](const char* name){ ImGui::SetWindowFocus(name); }, py::arg("name"));
//
//m.def("get_scroll_x", &ImGui::GetScrollX);
//m.def("get_scroll_y", &ImGui::GetScrollY);
//m.def("get_scroll_max_x", &ImGui::GetScrollMaxX);
//m.def("get_scroll_max_y", &ImGui::GetScrollMaxY);
//m.def("set_scroll_x", &ImGui::SetScrollX);
//m.def("set_scroll_y", &ImGui::SetScrollY);
//m.def("set_scroll_here", &ImGui::SetScrollHere, py::arg("center_y_ratio") = 0.5f);
//m.def("set_scroll_from_pos_y", &ImGui::SetScrollFromPosY, py::arg("pos_y"), py::arg("center_y_ratio") = 0.5f);
//m.def("set_keyboard_focus_here", &ImGui::SetKeyboardFocusHere, py::arg("offset") = 0.0f);
//
//m.def("push_style_color", [](ImGuiCol_ idx, const ImVec4& col){ ImGui::PushStyleColor((ImGuiCol)idx, col); });
//m.def("pop_style_color", &ImGui::PopStyleColor, py::arg("count") = 1);
//
//m.def("push_style_var", [](ImGuiStyleVar_ idx, float val){ ImGui::PushStyleVar((ImGuiStyleVar)idx, val); });
//m.def("push_style_var", [](ImGuiStyleVar_ idx, ImVec2 val){ ImGui::PushStyleVar((ImGuiStyleVar)idx, val); });
//m.def("pop_style_var", &ImGui::PopStyleVar, py::arg("count") = 1);
//
//m.def("push_item_width", &ImGui::PushItemWidth);
//m.def("pop_item_width", &ImGui::PopItemWidth);
//m.def("calc_item_width", &ImGui::CalcItemWidth);
//m.def("push_text_wrap_pos", &ImGui::PushTextWrapPos, py::arg("wrap_pos_x") = 0.0f);
//m.def("pop_text_wrap_pos", &ImGui::PopTextWrapPos);
//m.def("push_allow_keyboard_focus", &ImGui::PushAllowKeyboardFocus);
//m.def("pop_allow_keyboard_focus", &ImGui::PopAllowKeyboardFocus);
//m.def("push_button_repeat", &ImGui::PushButtonRepeat);
//m.def("pop_button_repeat", &ImGui::PopButtonRepeat);
//
//
//m.def("separator", &ImGui::Separator);
//m.def("same_line", &ImGui::SameLine, py::arg("local_pos_x") = 0.0f, py::arg("spacing_w") = -1.0f);
//m.def("new_line", &ImGui::NewLine);
//m.def("spacing", &ImGui::Spacing);
//m.def("dummy", &ImGui::Dummy);
//m.def("indent", &ImGui::Indent, py::arg("indent_w") = 0.0f);
//m.def("unindent", &ImGui::Unindent, py::arg("indent_w") = 0.0f);
//m.def("begin_group", &ImGui::BeginGroup);
//m.def("end_group", &ImGui::EndGroup);
//m.def("get_cursor_pos", &ImGui::GetCursorPos);
//m.def("get_cursor_pos_x", &ImGui::GetCursorPosX);
//m.def("get_cursor_pos_y", &ImGui::GetCursorPosY);
//m.def("set_cursor_pos", &ImGui::SetCursorPos);
//m.def("set_cursor_pos_x", &ImGui::SetCursorPosX);
//m.def("set_cursor_pos_y", &ImGui::SetCursorPosY);
//m.def("get_cursor_start_pos", &ImGui::GetCursorStartPos);
//m.def("get_cursor_screen_pos", &ImGui::GetCursorScreenPos);
//m.def("set_cursor_screen_pos", &ImGui::SetCursorScreenPos);
//m.def("align_first_text_height_to_widgets", &ImGui::AlignFirstTextHeightToWidgets);
//m.def("get_text_line_height", &ImGui::GetTextLineHeight);
//m.def("get_text_line_height_with_spacing", &ImGui::GetTextLineHeightWithSpacing);
//m.def("get_items_line_height_with_spacing", &ImGui::GetItemsLineHeightWithSpacing);
//
//m.def("columns", &ImGui::Columns, py::arg("count") = 1, py::arg("id") = nullptr, py::arg("border") = true);
//m.def("next_column", &ImGui::NextColumn);
//m.def("get_column_index", &ImGui::GetColumnIndex);
//m.def("get_column_offset", &ImGui::GetColumnOffset, py::arg("column_index") = -1);
//m.def("set_column_offset", &ImGui::SetColumnOffset, py::arg("column_index"), py::arg("offset_x"));
//m.def("get_column_width", &ImGui::GetColumnWidth, py::arg("column_index") = -1);
//m.def("set_column_width", &ImGui::SetColumnWidth, py::arg("column_index"), py::arg("column_width"));
//m.def("get_columns_count", &ImGui::GetColumnsCount);
//
//m.def("push_id_str", [](const char* str_id_begin, const char* str_id_end){ ImGui::PushID(str_id_begin, str_id_end); }, py::arg("str_id_begin"), py::arg("str_id_end") = nullptr);
//m.def("push_id_int", [](int int_id){ ImGui::PushID(int_id); } );
//m.def("pop_id", &ImGui::PopID);
//m.def("get_id_str", [](const char* str_id_begin, const char* str_id_end){ ImGui::GetID(str_id_begin, str_id_end); }, py::arg("str_id_begin"), py::arg("str_id_end") = nullptr);
//
//m.def("text", [](const char* text){ ImGui::Text("%s", text); });
//m.def("text_colored", [](const ImVec4& col, const char* text){ ImGui::TextColored(col, "%s", text); });
//m.def("text_disabled", [](const char* text){ ImGui::TextDisabled("%s", text); });
//m.def("text_wrapped", [](const char* text){ ImGui::TextWrapped("%s", text); });
//m.def("label_text", [](const char* label, const char* text){ ImGui::LabelText(label, "%s", text); });
//m.def("bullet_text", [](const char* text){ ImGui::BulletText("%s", text); });
//m.def("bullet", &ImGui::Bullet);
//
//m.def("button", &ImGui::Button, py::arg("label"), py::arg("size") = ImVec2(0,0));
//m.def("small_button", &ImGui::SmallButton);
//m.def("invisible_button", &ImGui::InvisibleButton);
//m.def("tree_node", [](const char* label){ return ImGui::TreeNode(label); }, py::arg("label"));
//m.def("tree_pop", &ImGui::TreePop);
//m.def("set_next_tree_node_open", &ImGui::SetNextTreeNodeOpen, py::arg("is_open"), py::arg("cond") = 0);
//m.def("collapsing_header", [](const char* label, ImGuiTreeNodeFlags flags){ return ImGui::CollapsingHeader(label, flags); }, py::arg("label"), py::arg("flags") = 0);
//m.def("checkbox", [](const char* label, Bool& v){ return ImGui::Checkbox(label, &v.value); });
//m.def("radio_button", [](const char* label, bool active){ return ImGui::RadioButton(label, active); });
//
//m.def("begin_combo", &ImGui::BeginCombo, py::arg("label"), py::arg("preview_value"), py::arg("flags") = 0);
//m.def("end_combo", &ImGui::EndCombo, "only call EndCombo() if BeginCombo() returns true!");
//m.def("combo", [](const char* label, Int& current_item, const std::vector<std::string>& items)
//{
//if (items.size() < 10)
//{
//const char* items_[10];
//for (int i = 0; i < (int)items.size(); ++i)
//{
//items_[i] = items[i].c_str();
//}
//return ImGui::Combo(label, &current_item.value, items_, (int)items.size());
//}
//else
//{
//const char** items_= new const char*[items.size()];
//for (int i = 0; i < (int)items.size(); ++i)
//{
//items_[i] = items[i].c_str();
//}
//bool result = ImGui::Combo(label, &current_item.value, items_, (int)items.size());
//delete[] items_;
//return result;
//}
//});
//
//m.def("input_text", [](const char* label, String& text, size_t buf_size, ImGuiInputTextFlags flags)
//{
//bool result = false;
//if (buf_size > 256)
//{
//char* buff = new char[buf_size];
//strncpy(buff, text.value.c_str(), buf_size);
//result = ImGui::InputText(label, buff, buf_size, flags);
//if (result)
//{
//text.value = buff;
//}
//delete[] buff;
//}
//else
//{
//char buff[256];
//strncpy(buff, text.value.c_str(), 256);
//result = ImGui::InputText(label, buff, buf_size, flags);
//if (result)
//{
//text.value = buff;
//}
//}
//return result;
//}, py::arg("label"), py::arg("text"), py::arg("buf_size"), py::arg("flags") = 0);
//m.def("input_text_multiline", [](const char* label, String& text, size_t buf_size, const ImVec2& size, ImGuiInputTextFlags flags)
//{
//bool result = false;
//if (buf_size > 256)
//{
//char* buff = new char[buf_size];
//strncpy(buff, text.value.c_str(), buf_size);
//result = ImGui::InputTextMultiline(label, buff, buf_size, size, flags);
//if (result)
//{
//text.value = buff;
//}
//delete[] buff;
//}
//else
//{
//char buff[256];
//strncpy(buff, text.value.c_str(), 256);
//result = ImGui::InputTextMultiline(label, buff, buf_size, size, flags);
//if (result)
//{
//text.value = buff;
//}
//}
//return result;
//}, py::arg("label"), py::arg("text"), py::arg("buf_size"), py::arg("size") = ImVec2(0,0), py::arg("flags") = 0);
//m.def("input_float", [](const char* label, Float& v, float step, float step_fast, int decimal_precision, ImGuiInputTextFlags flags)
//{
//return ImGui::InputFloat(label, &v.value, step, step_fast, decimal_precision, flags);
//}, py::arg("label"), py::arg("v"), py::arg("step") = 0.0f, py::arg("step_fast") = 0.0f, py::arg("decimal_precision") = -1, py::arg("flags") = 0);
//m.def("input_float2", [](const char* label, Float& v1, Float& v2, int decimal_precision, ImGuiInputTextFlags flags)
//{
//float v[2] = {v1.value, v2.value};
//bool result = ImGui::InputFloat2(label, v, decimal_precision, flags);
//v1.value = v[0];
//v2.value = v[1];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("decimal_precision") = -1, py::arg("flags") = 0);
//m.def("input_float3", [](const char* label, Float& v1, Float& v2, Float& v3, int decimal_precision, ImGuiInputTextFlags flags)
//{
//float v[3] = {v1.value, v2.value, v3.value};
//bool result = ImGui::InputFloat3(label, v, decimal_precision, flags);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("decimal_precision") = -1, py::arg("flags") = 0);
//m.def("input_float4", [](const char* label, Float& v1, Float& v2, Float& v3, Float& v4, int decimal_precision, ImGuiInputTextFlags flags)
//{
//float v[4] = {v1.value, v2.value, v3.value, v4.value};
//bool result = ImGui::InputFloat4(label, v, decimal_precision, flags);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//v4.value = v[3];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v4"), py::arg("decimal_precision") = -1, py::arg("flags") = 0);
//m.def("input_int", [](const char* label, Int& v, int step, int step_fast, ImGuiInputTextFlags flags)
//{
//return ImGui::InputInt(label, &v.value, step, step_fast, flags);
//}, py::arg("label"), py::arg("v"), py::arg("step") = 1, py::arg("step_fast") = 100, py::arg("flags") = 0);
//m.def("input_int2", [](const char* label, Int& v1, Int& v2, ImGuiInputTextFlags flags)
//{
//int v[2] = {v1.value, v2.value};
//bool result = ImGui::InputInt2(label, v, flags);
//v1.value = v[0];
//v2.value = v[1];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("flags") = 0);
//m.def("input_int3", [](const char* label, Int& v1, Int& v2, Int& v3, ImGuiInputTextFlags flags)
//{
//int v[3] = {v1.value, v2.value, v3.value};
//bool result = ImGui::InputInt3(label, v, flags);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("flags") = 0);
//m.def("input_int4", [](const char* label, Int& v1, Int& v2, Int& v3, Int& v4, ImGuiInputTextFlags flags)
//{
//int v[4] = {v1.value, v2.value, v3.value, v4.value};
//bool result = ImGui::InputInt4(label, v, flags);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//v4.value = v[3];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v4"), py::arg("flags") = 0);
//
//
//m.def("color_edit", [](const char* label, ImVec4& col)->bool
//{
//return ImGui::ColorEdit4(label, &col.x);
//});
//m.def("color_picker", [](const char* label, ImVec4& col)->bool
//{
//return ImGui::ColorPicker4(label, &col.x);
//});
//
//
//m.def("slider_float", [](const char* label, Float& v, float v_min, float v_max, const char* display_format, float power)
//{
//return ImGui::SliderFloat(label, &v.value, v_min, v_max, display_format, power);
//}, py::arg("label"), py::arg("v"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//m.def("slider_float2", [](const char* label, Float& v1, Float& v2, float v_min, float v_max, const char* display_format, float power)
//{
//float v[2] = {v1.value, v2.value};
//bool result = ImGui::SliderFloat2(label, v, v_min, v_max, display_format, power);
//v1.value = v[0];
//v2.value = v[1];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//m.def("slider_float3", [](const char* label, Float& v1, Float& v2, Float& v3, float v_min, float v_max, const char* display_format, float power)
//{
//float v[3] = {v1.value, v2.value, v3.value};
//bool result = ImGui::SliderFloat3(label, v, v_min, v_max, display_format, power);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//m.def("slider_float4", [](const char* label, Float& v1, Float& v2, Float& v3, Float& v4, float v_min, float v_max, const char* display_format, float power)
//{
//float v[4] = {v1.value, v2.value, v3.value, v4.value};
//bool result = ImGui::SliderFloat4(label, v, v_min, v_max, display_format, power);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//v4.value = v[3];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v4"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//
//m.def("v_slider_float", [](const char* label, const ImVec2& size, Float& v, float v_min, float v_max, const char* display_format, float power)
//{
//return ImGui::VSliderFloat(label, size, &v.value, v_min, v_max, display_format, power);
//}, py::arg("label"), py::arg("size"), py::arg("v"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//
//m.def("slider_angle", [](const char* label, Float& v_rad, float v_degrees_min, float v_degrees_max)
//{
//return ImGui::SliderAngle(label, &v_rad.value, v_degrees_min, v_degrees_max);
//}, py::arg("label"), py::arg("v_rad"), py::arg("v_degrees_min")=-360.0f, py::arg("v_degrees_max")=+360.0f);
//
//m.def("slider_int", [](const char* label, Int& v, int v_min, int v_max, const char* display_format)
//{
//return ImGui::SliderInt(label, &v.value, v_min, v_max, display_format);
//}, py::arg("label"), py::arg("v"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//m.def("slider_int2", [](const char* label, Int& v1, Int& v2, int v_min, int v_max, const char* display_format)
//{
//int v[2] = {v1.value, v2.value};
//bool result = ImGui::SliderInt2(label, v, v_min, v_max, display_format);
//v1.value = v[0];
//v2.value = v[1];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//m.def("slider_int3", [](const char* label, Int& v1, Int& v2, Int& v3, int v_min, int v_max, const char* display_format)
//{
//int v[3] = {v1.value, v2.value, v3.value};
//bool result = ImGui::SliderInt3(label, v, v_min, v_max, display_format);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//m.def("slider_int4", [](const char* label, Int& v1, Int& v2, Int& v3, Int& v4, int v_min, int v_max, const char* display_format)
//{
//int v[4] = {v1.value, v2.value, v3.value, v4.value};
//bool result = ImGui::SliderInt4(label, v, v_min, v_max, display_format);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//v4.value = v[3];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v4"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//
//
//m.def("v_slider_int", [](const char* label, const ImVec2& size, Int& v, int v_min, int v_max, const char* display_format)
//{
//return ImGui::VSliderInt(label, size, &v.value,  v_min, v_max, display_format);
//}, py::arg("label"), py::arg("size"), py::arg("v"), py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//
////
//m.def("drag_float", [](const char* label, Float& v, float v_speed, float v_min, float v_max, const char* display_format, float power)
//{
//return ImGui::DragFloat(label, &v.value, v_speed, v_min, v_max, display_format, power);
//}, py::arg("label"), py::arg("v"), py::arg("v_speed") = 1.0f, py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//m.def("drag_float2", [](const char* label, Float& v1, Float& v2, float v_speed, float v_min, float v_max, const char* display_format, float power)
//{
//float v[2] = {v1.value, v2.value};
//bool result = ImGui::DragFloat2(label, v, v_speed, v_min, v_max, display_format, power);
//v1.value = v[0];
//v2.value = v[1];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v_speed") = 1.0f, py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//m.def("drag_float3", [](const char* label, Float& v1, Float& v2, Float& v3, float v_speed, float v_min, float v_max, const char* display_format, float power)
//{
//float v[3] = {v1.value, v2.value, v3.value};
//bool result = ImGui::DragFloat3(label, v, v_speed, v_min, v_max, display_format, power);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v_speed") = 1.0f, py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//m.def("drag_float4", [](const char* label, Float& v1, Float& v2, Float& v3, Float& v4, float v_speed, float v_min, float v_max, const char* display_format, float power)
//{
//float v[4] = {v1.value, v2.value, v3.value, v4.value};
//bool result = ImGui::DragFloat4(label, v, v_speed, v_min, v_max, display_format, power);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//v4.value = v[3];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v4"), py::arg("v_speed") = 1.0f, py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.3f", py::arg("power") = 1.0f);
//
//m.def("drag_int", [](const char* label, Int& v, float v_speed, int v_min, int v_max, const char* display_format)
//{
//return ImGui::DragInt(label, &v.value, v_speed, v_min, v_max, display_format);
//}, py::arg("label"), py::arg("v"), py::arg("v_speed") = 1.0f, py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//m.def("drag_int2", [](const char* label, Int& v1, Int& v2, float v_speed, int v_min, int v_max, const char* display_format)
//{
//int v[2] = {v1.value, v2.value};
//bool result = ImGui::DragInt2(label, v, v_speed, v_min, v_max, display_format);
//v1.value = v[0];
//v2.value = v[1];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v_speed") = 1.0f, py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//m.def("drag_int3", [](const char* label, Int& v1, Int& v2, Int& v3, float v_speed, int v_min, int v_max, const char* display_format)
//{
//int v[3] = {v1.value, v2.value, v3.value};
//bool result = ImGui::DragInt3(label, v, v_speed, v_min, v_max, display_format);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v_speed") = 1.0f, py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//m.def("drag_int4", [](const char* label, Int& v1, Int& v2, Int& v3, Int& v4, float v_speed, int v_min, int v_max, const char* display_format)
//{
//int v[4] = {v1.value, v2.value, v3.value, v4.value};
//bool result = ImGui::DragInt4(label, v, v_speed, v_min, v_max, display_format);
//v1.value = v[0];
//v2.value = v[1];
//v3.value = v[2];
//v4.value = v[3];
//return result;
//}, py::arg("label"), py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("v4"), py::arg("v_speed") = 1.0f, py::arg("v_min"), py::arg("v_max"), py::arg("display_format") = "%.0f");
//
//m.def("plot_lines", [](
//const char* label,
//const std::vector<float>& values,
//int values_offset = 0,
//const char* overlay_text = NULL,
//float scale_min = FLT_MAX,
//float scale_max = FLT_MAX,
//		ImVec2 graph_size = ImVec2(0,0),
//int stride = sizeof(float))
//{
//ImGui::PlotLines(label, values.data(), (int)values.size(), values_offset, overlay_text, scale_min, scale_max, graph_size, stride);
//}
//, py::arg("label")
//, py::arg("values")
//, py::arg("values_offset") = 0
//, py::arg("overlay_text") = nullptr
//, py::arg("scale_min") = FLT_MAX
//, py::arg("scale_max") = FLT_MAX
//, py::arg("graph_size") = ImVec2(0,0)
//, py::arg("stride") = sizeof(float)
//);
//
//m.def("progress_bar", &ImGui::ProgressBar, py::arg("fraction"), py::arg("size_arg") = ImVec2(-1,0), py::arg("overlay") = nullptr);
//
//m.def("color_button", &ImGui::ColorButton, py::arg("desc_id"), py::arg("col"), py::arg("flags") = 0, py::arg("size") = ImVec2(0,0), "display a colored square/button, hover for details, return true when pressed.");
//
//m.def("selectable", [](
//std::string label,
//bool selected = false,
//		ImGuiSelectableFlags flags = 0,
//		ImVec2 size = ImVec2(0,0))->bool
//{
//return ImGui::Selectable(label.c_str(), selected, flags, size);
//}
//, py::arg("label")
//, py::arg("selected") = false
//, py::arg("flags") = 0
//, py::arg("size") = ImVec2(0,0)
//);
//
//m.def("selectable", [](
//std::string label,
//		Bool& selected,
//ImGuiSelectableFlags flags = 0,
//		ImVec2 size = ImVec2(0,0))->bool
//{
//return ImGui::Selectable(label.c_str(), &selected.value, flags, size);
//}
//, py::arg("label")
//, py::arg("selected")
//, py::arg("flags") = 0
//, py::arg("size") = ImVec2(0,0)
//);
//
//m.def("list_box_header", [](
//std::string label,
//		ImVec2 size = ImVec2(0,0))
//{
//ImGui::ListBoxHeader(label.c_str(), size);
//}
//, py::arg("label")
//, py::arg("size")
//);
//
//m.def("list_box_footer", &ImGui::ListBoxFooter);
//
//m.def("set_item_default_focus", &ImGui::SetItemDefaultFocus);
//m.def("set_keyboard_focus_here", &ImGui::SetKeyboardFocusHere);
//
//m.def("is_item_hovered", &ImGui::IsItemHovered);
//m.def("is_item_active", &ImGui::IsItemActive);
//m.def("is_item_focused", &ImGui::IsItemFocused);
//m.def("is_item_clicked", &ImGui::IsItemClicked);
//m.def("is_item_visible", &ImGui::IsItemVisible);
//m.def("is_item_edited", &ImGui::IsItemEdited);
//m.def("is_item_deactivated", &ImGui::IsItemDeactivated);
//m.def("is_item_deactivated_after_edit", &ImGui::IsItemDeactivatedAfterEdit);
//m.def("is_any_item_hovered", &ImGui::IsAnyItemHovered);
//m.def("is_any_item_active", &ImGui::IsAnyItemActive);
//m.def("is_any_item_focused", &ImGui::IsAnyItemFocused);
//
//m.def("get_item_rect_min", &ImGui::GetItemRectMin);
//m.def("get_item_rect_max", &ImGui::GetItemRectMax);
//m.def("get_item_rect_size", &ImGui::GetItemRectSize);
//
//m.def("set_item_allow_overlap", &ImGui::SetItemAllowOverlap);
//
//m.def("get_time", &ImGui::GetTime);
//m.def("get_frame_count", &ImGui::GetFrameCount);
//
//m.def("get_key_index", &ImGui::GetKeyIndex);
//m.def("is_key_down", &ImGui::IsKeyDown);
//m.def("is_key_pressed", &ImGui::IsKeyPressed);
//m.def("is_key_released", &ImGui::IsKeyReleased);
//m.def("get_key_pressed_amount", &ImGui::GetKeyPressedAmount);
//m.def("is_mouse_down", &ImGui::IsMouseDown);
//m.def("is_any_mouse_down", &ImGui::IsAnyMouseDown);
//m.def("is_mouse_clicked", &ImGui::IsMouseClicked);
//m.def("is_mouse_double_clicked", &ImGui::IsMouseDoubleClicked);
//m.def("is_mouse_released", &ImGui::IsMouseReleased);
//m.def("is_mouse_dragging", &ImGui::IsMouseDragging);
//m.def("is_mouse_hovering_rect", &ImGui::IsMouseHoveringRect);
//m.def("is_mouse_pos_valid", &ImGui::IsMousePosValid);
//m.def("get_mouse_pos", &ImGui::GetMousePos);
//m.def("get_mouse_pos_on_opening_current_popup", &ImGui::GetMousePosOnOpeningCurrentPopup);
//m.def("get_mouse_drag_delta", &ImGui::GetMouseDragDelta);
//m.def("reset_mouse_drag_delta", &ImGui::ResetMouseDragDelta);
//
//m.def("capture_keyboard_from_app", &ImGui::CaptureKeyboardFromApp);
//m.def("capture_mouse_from_app", &ImGui::CaptureMouseFromApp);
//
//py::enum_<ImGuiDragDropFlags_>(m, "DragDropFlags")
//.value("SourceNoPreviewTooltip", ImGuiDragDropFlags_SourceNoPreviewTooltip)
//.value("SourceNoDisableHover", ImGuiDragDropFlags_SourceNoDisableHover)
//.value("SourceNoHoldToOpenOthers", ImGuiDragDropFlags_SourceNoHoldToOpenOthers)
//.value("SourceAllowNullID", ImGuiDragDropFlags_SourceAllowNullID)
//.value("SourceExtern", ImGuiDragDropFlags_SourceExtern)
//.value("SourceAutoExpirePayload", ImGuiDragDropFlags_SourceAutoExpirePayload)
//
//.value("AcceptBeforeDelivery", ImGuiDragDropFlags_AcceptBeforeDelivery)
//.value("AcceptNoDrawDefaultRect", ImGuiDragDropFlags_AcceptNoDrawDefaultRect)
//.value("AcceptNoPreviewTooltip", ImGuiDragDropFlags_AcceptNoPreviewTooltip)
//.value("AcceptPeekOnly", ImGuiDragDropFlags_AcceptPeekOnly)
//
//.export_values();
//
//m.def("begin_drag_drop_source", &ImGui::BeginDragDropSource);
//// todo:
////m.def("set_drag_drop_payload", &ImGui::SetDragDropPayload);
//m.def("set_drag_drop_payload_string", [](std::string data){ImGui::SetDragDropPayload("string",data.c_str(), data.size());});
//m.def("end_drag_drop_source", &ImGui::EndDragDropSource);
//m.def("begin_drag_drop_target", &ImGui::BeginDragDropTarget);
//// todo:
////m.def("accept_drag_drop_payload", &ImGui::AcceptDragDropPayload);
//m.def("accept_drag_drop_payload_string_preview", [](ImGuiDragDropFlags flags = 0)->std::string{
//auto payload = ImGui::AcceptDragDropPayload("string", flags);
//if (!payload->IsDataType("string") || !payload->Data)
//return "";
//if (payload->IsPreview())
//return std::string(static_cast<char*>(payload->Data), payload->DataSize);
//else
//return "";
//});
//m.def("accept_drag_drop_payload_string", [](ImGuiDragDropFlags flags = 0)->std::string{
//auto payload = ImGui::AcceptDragDropPayload("string", flags);
//if (!payload->IsDataType("string") || !payload->Data)
//return "";
//if (payload->IsDelivery())
//return std::string(static_cast<char*>(payload->Data), payload->DataSize);
//else
//return "";
//});
//
//m.def("end_drag_drop_target", &ImGui::EndDragDropTarget);
//
//m.def("push_clip_rect", &ImGui::PushClipRect);
//m.def("pop_clip_rect", &ImGui::PopClipRect);
//
//m.def("add_line", &AddLine, py::arg("a"), py::arg("b"), py::arg("col"), py::arg("thickness") = 1.0f);
//m.def("add_rect", &AddRect, py::arg("a"), py::arg("b"), py::arg("col"), py::arg("rounding") = 0.0f, py::arg("rounding_corners_flags") = ImDrawCornerFlags_All, py::arg("thickness") = 1.0f);
//m.def("add_rect_filled", &AddRectFilled, py::arg("a"), py::arg("b"), py::arg("col"), py::arg("rounding") = 0.0f, py::arg("rounding_corners_flags") = ImDrawCornerFlags_All);
//m.def("add_rect_filled_multicolor", &AddRectFilledMultiColor, py::arg("a"), py::arg("b"), py::arg("col_upr_left"), py::arg("col_upr_right"), py::arg("col_bot_right"), py::arg("col_bot_lefs"));
//m.def("add_quad", &AddQuad, py::arg("a"), py::arg("b"), py::arg("c"), py::arg("d"), py::arg("col"), py::arg("thickness") = 1.0f);
//m.def("add_quad_filled", &AddQuadFilled, py::arg("a"), py::arg("b"), py::arg("c"), py::arg("d"), py::arg("col"));
//m.def("add_triangle", &AddTriangle, py::arg("a"), py::arg("b"), py::arg("c"), py::arg("col"), py::arg("thickness") = 1.0f);
//m.def("add_triangle_filled", &AddTriangleFilled, py::arg("a"), py::arg("b"), py::arg("c"), py::arg("col"));
//m.def("add_circle", &AddCircle, py::arg("centre"), py::arg("radius"), py::arg("col"), py::arg("num_segments") = 12, py::arg("thickness") = 1.0f);
//m.def("add_circle_filled", &AddCircleFilled, py::arg("centre"), py::arg("radius"), py::arg("col"), py::arg("num_segments") = 12);
//m.def("add_bezier_curve", &AddBezierCurve, py::arg("pos0"), py::arg("cp0"), py::arg("cp1"), py::arg("pos1"), py::arg("col"), py::arg("thickness"), py::arg("num_segments") = 0);
//
//m.def("path_clear", &PathClear);
//m.def("path_line_to", &PathLineTo, py::arg("pos"));
//m.def("path_fill_convex", &PathFillConvex, py::arg("col"));
//m.def("path_stroke", &PathStroke, py::arg("col"), py::arg("closed"), py::arg("thickness"));
//
//m.def("add_font_from_file_ttf", [](
//std::string filename,
//int size_pixels = 32)
//{
//return ImGui::GetIO().Fonts->AddFontFromFileTTF(filename.c_str(), size_pixels);
//}
//, py::arg("filename")
//, py::arg("size_pixels"), py::return_value_policy::reference);
//
//m.def("push_font", &ImGui::PushFont);
//m.def("pop_font", &ImGui::PopFont);
//m.def("get_font", &ImGui::GetFont);
//
//py::class_<ImFont>(m, "Font")
//.def(py::init())
//;
//
//m.def("set_display_framebuffer_scale",[](float scale){
//ImGui::GetIO().DisplayFramebufferScale = ImVec2(scale,scale);
//}, py::arg("scale"));
//m.def("get_display_framebuffer_scale",[](){
//return ImGui::GetIO().DisplayFramebufferScale;
//});
//
//m.def("set_font_global_scale",[](float scale){
//ImGui::GetIO().FontGlobalScale = scale;
//}, py::arg("scale"));
//m.def("get_font_global_scale",[](){
//return ImGui::GetIO().FontGlobalScale;
//});
//typedef void (*FImage_texId_wSize)(GLuint, ImVec2&);
//m.def("image", (FImage_texId_wSize)([](GLuint textureId, ImVec2& size)
//{
//ImGui::Image(reinterpret_cast<ImTextureID>(textureId), size);
//}));
//typedef void (*FImage_wSize)(Image*, ImVec2&);
//m.def("image", (FImage_wSize)([](Image* im, ImVec2& size)
//{
//ImGui::Image(reinterpret_cast<ImTextureID>(im->GetHandle()), size);
//}));
//typedef void (*FImage)(Image*);
//m.def("image", (FImage)([](Image* im)
//{
//ImGui::Image(reinterpret_cast<ImTextureID>(im->GetHandle()), ImVec2(im->m_width, im->m_height));
//}));
//
//m.def("image_button", &ImGui::ImageButton);
//
//m.attr("key_left_shift") = py::int_(GLFW_KEY_LEFT_SHIFT);
//m.attr("key_left_control") = py::int_(GLFW_KEY_LEFT_CONTROL);
//m.attr("key_left_alt") = py::int_(GLFW_KEY_LEFT_ALT);
//m.attr("key_left_super") = py::int_(GLFW_KEY_LEFT_SUPER);
//m.attr("key_right_shift") = py::int_(GLFW_KEY_RIGHT_SHIFT);
//m.attr("key_right_control") = py::int_(GLFW_KEY_RIGHT_CONTROL);
//m.attr("key_right_alt") = py::int_(GLFW_KEY_RIGHT_ALT);
//m.attr("key_right_super") = py::int_(GLFW_KEY_RIGHT_SUPER);
}
