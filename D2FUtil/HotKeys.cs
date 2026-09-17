namespace D2Chain.D2FUtil;

/// <summary>One entry in a hotkey drop-down: a Windows virtual-key code and what to call it.</summary>
internal sealed record HotKey(int Vk, string Name)
{
    public override string ToString() => Name;

    /// <summary>
    /// The keys offered for binding. Deliberately excludes the keys Diablo II itself uses for
    /// movement, chat and the belt; the function keys are offered because the module swallows the
    /// keys it handles, so binding one does not also trigger the game's skill slot.
    /// </summary>
    public static readonly HotKey[] All = Build();

    static HotKey[] Build()
    {
        var list = new List<HotKey>
        {
            new(0x21, "Page Up"), new(0x22, "Page Down"), new(0x24, "Home"), new(0x23, "End"),
            new(0x2D, "Insert"), new(0x2E, "Delete"),
        };
        for (int i = 1; i <= 12; i++) list.Add(new(0x6F + i, $"F{i}"));
        for (int i = 0; i <= 9; i++) list.Add(new(0x60 + i, $"Numpad {i}"));
        list.Add(new(0x6A, "Numpad *"));
        list.Add(new(0x6D, "Numpad -"));
        list.Add(new(0x6B, "Numpad +"));
        list.Add(new(0x6F, "Numpad /"));
        list.Add(new(0xC0, "` (backtick)"));
        list.Add(new(0xDB, "[")); list.Add(new(0xDD, "]"));
        list.Add(new(0xBA, ";")); list.Add(new(0xDE, "'"));
        list.Add(new(0xBC, ",")); list.Add(new(0xBE, "."));
        return list.ToArray();
    }

    public static HotKey For(int vk) => Array.Find(All, k => k.Vk == vk) ?? new HotKey(vk, $"key 0x{vk:X2}");
}
