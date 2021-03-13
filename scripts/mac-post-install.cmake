cmake_minimum_required(VERSION 2.8.12)

# We want to explicitly symlink the newly installed app bundle
# such that the icon and executable match up correctly.
execute_process(COMMAND rm -f /usr/local/bin/tev)
execute_process(COMMAND ln -s /Applications/tev.app/Contents/Resources/mac-run-tev.sh /usr/local/bin/tev)
