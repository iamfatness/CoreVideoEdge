using System.Windows.Input;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;

namespace CoreVideoEdge.Desktop.Controls;

// A talk key: press = key down, release (or losing the pointer) = key up.
// A Button's Click fires on release, which is the opposite of what a talk key
// needs, so the pointer is handled here and Click never fires. Space and Enter
// are left to bubble: Space is ALL CALL on the Comms screen (ZComms model), and
// a focused key must not swallow it.
public sealed class TalkKey : Button
{
    public static readonly StyledProperty<ICommand?> PressCommandProperty =
        AvaloniaProperty.Register<TalkKey, ICommand?>(nameof(PressCommand));
    public static readonly StyledProperty<ICommand?> ReleaseCommandProperty =
        AvaloniaProperty.Register<TalkKey, ICommand?>(nameof(ReleaseCommand));

    private bool _down;

    protected override Type StyleKeyOverride => typeof(Button);

    public ICommand? PressCommand { get => GetValue(PressCommandProperty); set => SetValue(PressCommandProperty, value); }
    public ICommand? ReleaseCommand { get => GetValue(ReleaseCommandProperty); set => SetValue(ReleaseCommandProperty, value); }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        if (!IsEffectivelyEnabled || !e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        e.Pointer.Capture(this);
        e.Handled = true;
        if (_down) return;
        _down = true;
        PressCommand?.Execute(CommandParameter);
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        e.Handled = true;
        Up();
        e.Pointer.Capture(null);
    }

    protected override void OnPointerCaptureLost(PointerCaptureLostEventArgs e)
    {
        base.OnPointerCaptureLost(e);
        Up();
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        if (e.Key is Key.Space or Key.Enter) return;  // bubble to the screen
        base.OnKeyDown(e);
    }

    protected override void OnKeyUp(KeyEventArgs e)
    {
        if (e.Key is Key.Space or Key.Enter) return;
        base.OnKeyUp(e);
    }

    private void Up()
    {
        if (!_down) return;
        _down = false;
        ReleaseCommand?.Execute(CommandParameter);
    }
}
