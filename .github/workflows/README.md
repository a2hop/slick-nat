# GitHub Actions Workflows

This directory contains GitHub Actions workflows for the Slick NAT project.

## build-and-release.yml

Automatically builds and releases the Slick NAT Debian package.

### Triggers
- Push to main/master branches
- Pull requests to main/master branches
- Manual workflow dispatch

### What it does
1. **Environment Setup**: Installs required build dependencies
2. **Version Detection**: Extracts version from source code
3. **Package Building**: Uses the existing `pkg/deb/build-deb.sh` script
4. **Release Upload**: Uploads package to the latest GitHub release
5. **Artifact Fallback**: If no release exists, uploads as workflow artifacts

### Outputs
- **Debian Package**: `slick-nat-dkms_VERSION_ARCHITECTURE.deb`
- **Management Script**: `slnat` (standalone script)
- **Build Summary**: Detailed information in workflow summary

### Usage

#### For Releases
1. Create a GitHub release (manually or via other workflows)
2. Push changes to main/master branch
3. The workflow will automatically build and upload the package to the latest release

#### For Development
- The workflow runs on pull requests and provides build verification
- If no release exists, artifacts are uploaded with 30-day retention

### Configuration

#### Version Detection
The workflow extracts version from `src/slick-nat.c`:
```c
MODULE_VERSION("0.0.3");
```

#### Architecture Support
- Automatically detects build architecture using `dpkg --print-architecture`
- Generates architecture-specific package names (e.g., `_amd64.deb`, `_arm64.deb`)

#### Asset Management
- Automatically replaces existing assets in releases
- Handles both the Debian package and management script
- Provides detailed logging of upload process

### Dependencies
- Ubuntu latest runner
- Standard Debian packaging tools
- DKMS and build dependencies

### Secrets Required
- `GITHUB_TOKEN`: Automatically provided by GitHub Actions
- No additional secrets needed

### Troubleshooting

#### Build Failures
- Check that `pkg/deb/build-deb.sh` is executable
- Verify all required source files are present
- Check workflow logs for specific error messages

#### Upload Failures
- Verify repository has releases
- Check token permissions (should be automatic)
- Ensure release is not in draft state

#### Version Issues
- Ensure `MODULE_VERSION` is properly defined in `src/slick-nat.c`
- Check version format matches semver expectations
- Verify version consistency across files

### Example Usage

After workflow completion, users can install with:
```bash
# Download from release
wget https://github.com/a2hop/slick-nat/releases/download/v0.0.3/slick-nat-dkms_0.0.3_amd64.deb

# Install package
sudo dpkg -i slick-nat-dkms_0.0.3_amd64.deb
sudo apt-get install -f

# Verify installation
lsmod | grep slick_nat
slnat status
```

### Future Enhancements
- Multi-architecture builds (ARM64, i386)
- Cross-compilation support
- Automated testing before release
- Integration with package repositories
- Signing of packages
