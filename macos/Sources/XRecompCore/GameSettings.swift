import Foundation

/// A title's settings over the Default settings, with the Android launcher's per-game semantics (XBA-188,
/// GameSettings.java, docs/android/app-shell.md « Settings per game »), in the title's own recomp.cfg:
///
/// - a key the title's file does not set follows Default settings;
/// - `KEY=VALUE` is the title's own value;
/// - `KEY=` (empty) is the runtime's default for this title even when Default settings set another (the choices that
///   write no line in Default settings: Off, Auto, 1x...). ChildEnvironment.game unsets such a variable.
/// - an option of several keys is the title's own when the file sets any of them; the keys it does not set keep
///   Default settings' values.
public struct GameSettings: Sendable {
    public var game: RecompConfig
    public let defaults: RecompConfig

    public init(game: RecompConfig, defaults: RecompConfig) {
        self.game = game
        self.defaults = defaults
    }

    /// The option follows Default settings: the title's file sets none of its keys.
    public func follows(_ option: ConfigOption) -> Bool {
        option.keys.allSatisfy { game.get($0) == nil }
    }

    /// The value the game gets for key: the title's line (`KEY=`: the runtime default, nil), else Default settings'.
    public func effective(_ key: String) -> String? {
        if let v = game.get(key) { return v.isEmpty ? nil : v }
        if let v = defaults.get(key), !v.isEmpty { return v }
        return nil
    }

    /// The choice the game gets, or nil when the values are not one the list offers.
    public func effectiveChoiceIndex(_ option: ConfigOption) -> Int? {
        option.choices.firstIndex { c in
            option.keys.enumerated().allSatisfy { k, key in c.values[k] == effective(key) }
        }
    }

    /// The title's own choice: nil when it follows Default settings or holds values the list does not offer.
    public func ownChoiceIndex(_ option: ConfigOption) -> Int? {
        follows(option) ? nil : effectiveChoiceIndex(option)
    }

    /// The label of Default settings' choice for the option (« Custom » for values the list does not offer).
    public func defaultsLabel(_ option: ConfigOption) -> String {
        defaults.choiceIndex(option).map { option.choices[$0].label } ?? "Custom"
    }

    /// The title's own choice: every key written, a runtime default as `KEY=`.
    public mutating func setOwn(_ option: ConfigOption, _ choice: ConfigOption.Choice) {
        for (k, key) in option.keys.enumerated() { game.set(key, choice.values[k] ?? "") }
    }

    /// Back to Default settings: the option's lines removed.
    public mutating func follow(_ option: ConfigOption) {
        for key in option.keys { game.set(key, nil) }
    }

    public mutating func followAll(_ options: [ConfigOption] = OptionTable.all) {
        for o in options { follow(o) }
    }

    /// True when every option of the table follows Default settings.
    public func allFollow(_ options: [ConfigOption] = OptionTable.all) -> Bool {
        options.allSatisfy { follows($0) }
    }
}
