# ITCHy VM Link Test

This directory is intentionally separate from the parser implementation in the project root. It contains a notebook and small scripts for creating a two-node FABRIC test where one VM sends the gzipped binary feed over a same-site L2Bridge data path and the other VM builds/runs the ITCHy parser.

## Workflow

1. Edit `config.env` if you want different slice/node names or VM sizing.
2. Open `ITCHy_VM_Link_Test.ipynb` in JupyterLab.
3. Run cells in order:
   - local preflight
   - create draft slice
   - submit/wait
   - install VM dependencies
   - upload source/data
   - build parser on receiver
   - transfer feed sender -> receiver and parse
   - collect results
4. Run cleanup only when you intentionally want to delete the FABRIC slice.

## Design Notes

- The existing source files in the project root are not modified by this workflow.
- `data/tvagg.gz` stays local until `04_upload_project.sh` copies it to the sender VM.
- The receiver VM does not receive the data file through LoomAI scp; it receives it over the same-site L2Bridge data path from the sender.
- The first implementation transfers the compressed file and then runs the parser on the received gzip. That matches the current parser API. A later parser enhancement can add stdin/raw-stream mode for true parse-while-receiving behavior.
- `08_cleanup_slice.sh` requires `CONFIRM_DELETE=yes` to prevent accidental deletion.

## Current Test Mode

The active scaffold uses a same-site L2Bridge with static VM-side addresses (`SENDER_DATA_IP` and `RECEIVER_DATA_IP` in `config.env`). This avoids depending on FABNetv4 address assignment for the first integration test.
