using Avalonia.Controls;
using CoreVideoEdge.Desktop.ViewModels;

namespace CoreVideoEdge.Desktop.Views;

public partial class RecordView : UserControl
{
    public RecordView()
    {
        InitializeComponent();
        // Two columns at >= 1200 px, stacked below that.
        SizeChanged += (_, e) =>
        {
            var wide = e.NewSize.Width >= 1200;
            Columns.ColumnDefinitions = wide ? new ColumnDefinitions("380,16,*") : new ColumnDefinitions("*");
            Grid.SetColumn(RightPane, wide ? 2 : 0);
            Grid.SetRow(RightPane, wide ? 0 : 1);
            RightPane.Margin = wide ? default : new Avalonia.Thickness(0, 16, 0, 0);
        };
        PrefixBox.GotFocus += (_, _) => (DataContext as RecordViewModel)?.SetPrefixEditing(true);
        PrefixBox.LostFocus += async (_, _) =>
        {
            if (DataContext is not RecordViewModel vm) return;
            vm.SetPrefixEditing(false);
            await vm.SavePrefixCommand.ExecuteAsync();
        };
    }
}
