using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using CoreVideoEdge.Desktop.ViewModels;
using CoreVideoEdge.Desktop.Views;

namespace CoreVideoEdge.Desktop;

// macOS menu bar. The application menu (named from Application.Name /
// CFBundleName) gets About and Settings; Avalonia's native side appends the
// standard Services, Hide, Hide Others, Show All and Quit (Cmd+Q) items. Quit
// goes through the window's OnClosing, so a recording take is confirmed and the
// node finalizes its files exactly like closing the window.
// Window menus: View (the four screens, Cmd+1..4, the same commands as the
// window key bindings, which are idempotent) and Help (log folder).
// Windows has no menu bar; nothing here runs there.
internal static class MacMenus
{
    public static void Install(Application app, MainWindow window, MainViewModel vm)
    {
        var about = new NativeMenuItem("About CoreVideo Edge");
        about.Click += (_, _) => new AboutWindow().ShowDialog(window);
        var settings = new NativeMenuItem("Settings…")
        {
            Gesture = new KeyGesture(Key.OemComma, KeyModifiers.Meta),
            Command = vm.SelectScreenCommand,
            CommandParameter = Screen.Record,
            ToolTip = "Record folder, file prefix and format",
        };
        NativeMenu.SetMenu(app, new NativeMenu { about, new NativeMenuItemSeparator(), settings });

        var view = new NativeMenu();
        var keys = new[] { Key.D1, Key.D2, Key.D3, Key.D4 };
        foreach (var screen in Enum.GetValues<Screen>())
        {
            view.Add(new NativeMenuItem(screen.ToString())
            {
                Gesture = new KeyGesture(keys[(int)screen], KeyModifiers.Meta),
                Command = vm.SelectScreenCommand,
                CommandParameter = screen,
            });
        }
        var help = new NativeMenu
        {
            new NativeMenuItem("Open Log Folder") { Command = vm.OpenLogFolderCommand },
        };
        NativeMenu.SetMenu(window, new NativeMenu
        {
            new NativeMenuItem("View") { Menu = view },
            new NativeMenuItem("Help") { Menu = help },
        });
    }
}
