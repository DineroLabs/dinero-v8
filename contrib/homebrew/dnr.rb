class Dnr < Formula
  desc "DineroCoin CLI - Production-grade blockchain control cockpit"
  homepage "https://github.com/dinerocoin/dinerocoin"
  version "1.0.0"
  license "MIT"

  if Hardware::CPU.arm?
    url "https://github.com/dinerocoin/dinerocoin/releases/download/v1.0.0/dinero-cli-v1.0.0-darwin-arm64.tar.gz"
    sha256 "REPLACE_WITH_ARM64_SHA256"
  else
    url "https://github.com/dinerocoin/dinerocoin/releases/download/v1.0.0/dinero-cli-v1.0.0-darwin-x86_64.tar.gz"
    sha256 "REPLACE_WITH_X86_64_SHA256"
  end

  def install
    bin.install "dinero-cli"
    
    # Install shell completions
    bash_completion.install "contrib/completions/dinero-cli.bash" => "dinero-cli"
    zsh_completion.install "contrib/completions/_dinero-cli"
    
    # Install man page if available
    if File.exist?("dinero-cli.1")
      man1.install "dinero-cli.1"
    end
  end

  test do
    system "#{bin}/dinero-cli", "--version"
    assert_match "dinero-cli 1.0.0", shell_output("#{bin}/dinero-cli --version")
    assert_match "din.cli.v1", shell_output("#{bin}/dinero-cli --version")
  end

  def caveats
    <<~EOS
      DineroCoin CLI has been installed!
      
      Quick start:
        # Create profile directory
        mkdir -p ~/.dinero-cli
        
        # Copy example profile
        cp #{HOMEBREW_PREFIX}/share/doc/din/examples/profiles.json ~/.dinero-cli/
        
        # Test connection (requires running dinerod)
        dinero-cli --profile dev --nodeinfo
      
      Documentation:
        #{homepage}/blob/main/docs/CLI_PROFILES.md
        #{homepage}/blob/main/docs/CLI_OPERATIONAL_RUNBOOK.md
    EOS
  end
end
