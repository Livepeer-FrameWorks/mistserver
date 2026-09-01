# YOLO26 AGPL distribution profile

MistServer itself remains available under the Unlicense/0BSD terms described in
`COPYING.md`. The release-default YOLO26n trained model is separately offered by
Ultralytics under AGPL-3.0. Ultralytics' published licensing position treats an application
using its code or trained models as the larger covered work. This release therefore takes
the open-source route and does not depend on an enterprise grant.

For any release channel that includes, mirrors, automatically deploys, or operates the
YOLO26n pack as part of the MistServer ONNX offering:

1. Convey the combined ONNX offering under AGPL-3.0 and include the complete, unmodified
   AGPL-3.0 license text. The permissive rights to MistServer, ONNX Runtime, and OpenCV
   remain intact; this additional distribution condition is for the combined YOLO offering.
2. Publish the complete corresponding source for the exact deployed version: MistServer,
   local modifications, model provisioning/export scripts, build and installation scripts,
   configuration needed to reproduce the deployment, and the corresponding YOLO model
   weights/source form customarily used to modify them.
3. Preserve copyright, license, model-card, provenance, and modification notices. Do not
   impose contractual or technical restrictions that contradict recipients' AGPL rights.
4. If users interact with the modified program over a network, provide a prominent way for
   those users to obtain the complete corresponding source for that running version.
5. Keep the source offer available for every shipped version and verify that release URLs
   are durable before publication.

Merely omitting the weights from the binary archive is not the compliance strategy for the
default auto-provisioned product. The AGPL lane above is. A distributor unwilling to meet it
must disable/remove the YOLO default and choose a model with compatible terms; this project
does not assume an enterprise license.

The authoritative inputs for this policy are the upstream
[Ultralytics license page](https://www.ultralytics.com/license) and the
[GNU AGPL-3.0 text](https://www.gnu.org/licenses/agpl-3.0.html). Re-check both when promoting
a new YOLO artifact or changing how the model and server are distributed.
