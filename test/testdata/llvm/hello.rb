puts(<<~EOS)
👋 Hey there! Heads up that this is not a release build of sorbet.
Release builds are faster and more well-supported by the Sorbet team.
Check out the README to learn how to build Sorbet in release mode.
To forcibly silence this error, either pass --silence-dev-message,
or set SORBET_SILENCE_DEV_MESSAGE=1 in your shell environment.

No errors! Great job.
EOS

puts "hello world"
