#pragma once

// The precompiled header, which for a C++/WinRT project is not merely a build-time optimisation:
// the projection headers are large enough that compiling them per translation unit turns a
// twenty-second build into a several-minute one.

#include <windows.h>
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// windows.h defines GetCurrentTime as a macro, and Microsoft::UI::Xaml::Media::Animation::Storyboard
// has a method by that name. Left defined, every XAML header that mentions it fails to compile.
// This #undef is in the stock template for exactly this reason and is not safe to drop.
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>

// The MRU list, which is how a file the user chose once can be reopened later without asking for
// it again, and the drag-and-drop formats.
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.ApplicationModel.Activation.h>

// AppInstance, which is what makes this program single-instance and what carries a file activation
// to the copy already running. In Microsoft.Windows.*, not Windows.*: the ApplicationModel one is
// the UWP lifecycle and knows nothing about a packaged desktop process.
#include <winrt/Microsoft.Windows.AppLifecycle.h>

#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>

// Not optional despite Microsoft.UI.Xaml.h forward-declaring the types: without this the delegate
// constructors are declared and never defined, and a lambda handed to PointerEventHandler compiles
// and then fails at link with an unresolved external naming the lambda.
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>

// MicaBackdrop lives in Microsoft.UI.Xaml.Media; MicaKind and the controller that answers whether
// the material is available at all live in the composition namespace. Both are needed to ask before
// setting, rather than setting and hoping.
#include <winrt/Microsoft.UI.Composition.SystemBackdrops.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Navigation.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>

// wil::resume_foreground for a Microsoft::UI::Dispatching::DispatcherQueue. C++/WinRT ships
// resume_foreground only for the Windows.* dispatchers -- the Microsoft.UI.* one lives in the
// Windows App SDK, which the projection cannot know about -- so without this there is no supported
// way to co_await your way back onto the UI thread after a background hop.
#include <wil/cppwinrt_helpers.h>
